#define DOCTEST_CONFIG_IMPLEMENT

#include "doctest/doctest.h"
#include <asio/any_completion_handler.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <rest_rpc/rpc_client.hpp>
#include <rest_rpc/rpc_server.hpp>

#include <optional>
#include <vector>

using namespace rest_rpc;

int add(int a, int b) { return a + b; }

namespace user_codec {
struct throwing_value {};

inline std::string serialize(rest_adl_tag, throwing_value) {
  throw std::runtime_error("response serialization failed");
}

inline std::string serialize(rest_adl_tag, std::tuple<throwing_value &&>) {
  throw std::runtime_error("request serialization failed");
}
} // namespace user_codec

int v2_accept_throwing(user_codec::throwing_value) { return 0; }
int v2_bad_result() { return 0; }

class failing_write_stream {
public:
  using executor_type = asio::any_io_executor;

  failing_write_stream(executor_type executor, size_t successful_writes)
      : executor_(std::move(executor)), successful_writes_(successful_writes) {}

  executor_type get_executor() const { return executor_; }

  template <typename ConstBufferSequence, typename CompletionToken>
  auto async_write_some(const ConstBufferSequence &buffers,
                        CompletionToken &&token) {
    const auto size = asio::buffer_size(buffers);
    const bool fail = successful_writes_ == 0;
    if (!fail) {
      --successful_writes_;
    }
    return asio::async_initiate<CompletionToken, void(std::error_code, size_t)>(
        [this, size, fail](auto handler) mutable {
          auto timer = std::make_shared<asio::steady_timer>(executor_);
          timer->expires_after(std::chrono::milliseconds(100));
          timer->async_wait([timer, size, fail, handler = std::move(handler)](
                                std::error_code ec) mutable {
            if (ec) {
              std::move(handler)(ec, 0);
            } else if (fail) {
              std::move(handler)(
                  asio::error::make_error_code(asio::error::connection_reset),
                  0);
            } else {
              std::move(handler)({}, size);
            }
          });
        },
        token);
  }

  template <typename MutableBufferSequence, typename CompletionToken>
  auto async_read_some(const MutableBufferSequence &, CompletionToken &&token) {
    return asio::async_initiate<CompletionToken, void(std::error_code, size_t)>(
        [this](auto handler) mutable {
          read_timer_ = std::make_shared<asio::steady_timer>(executor_);
          read_timer_->expires_after(std::chrono::seconds(5));
          read_timer_->async_wait(
              [timer = read_timer_,
               handler = std::move(handler)](std::error_code ec) mutable {
                if (!ec) {
                  ec = asio::error::make_error_code(asio::error::eof);
                }
                std::move(handler)(ec, 0);
              });
        },
        token);
  }

  void shutdown(asio::ip::tcp::socket::shutdown_type, std::error_code &ec) {
    ec.clear();
  }

  void close(std::error_code &ec) {
    if (read_timer_) {
      read_timer_->cancel();
    }
    ec.clear();
  }

private:
  executor_type executor_;
  size_t successful_writes_;
  std::shared_ptr<asio::steady_timer> read_timer_;
};

struct failing_write_transport {
  failing_write_transport(asio::any_io_executor executor,
                          size_t successful_writes)
      : impl_(std::move(executor), successful_writes) {}

  auto get_executor() const { return impl_.get_executor(); }

  failing_write_stream impl_;
  bool has_closed_ = false;
};

asio::awaitable<int> v2_delay(int delay_ms, int value) {
  asio::steady_timer timer(co_await asio::this_coro::executor);
  timer.expires_after(std::chrono::milliseconds(delay_ms));
  co_await timer.async_wait(asio::use_awaitable);
  co_return value;
}

std::string v2_echo_text(std::string value) { return value; }

void v2_notify(int value) { CHECK(value == 9); }

int v2_throw() { throw std::runtime_error("v2 handler error"); }

asio::awaitable<int> v2_manual_pack_throw(rpc_context context) {
  co_await context.response(user_codec::throwing_value{});
  co_return 0;
}

asio::awaitable<int> v2_manual_delay(rpc_context context, int delay_ms,
                                     int value) {
  auto copied_context = context;
  asio::steady_timer timer(co_await asio::this_coro::executor);
  timer.expires_after(std::chrono::milliseconds(delay_ms));
  co_await timer.async_wait(asio::use_awaitable);
  auto ec = co_await context.response(value);
  CHECK_FALSE(ec);
  auto duplicate = co_await copied_context.response(value);
  CHECK(duplicate == rpc_errc::has_response);
  co_return 0;
}

int v2_detached_response(rpc_context context, int delay_ms, int value) {
  auto executor = context.get_executor();
  asio::co_spawn(
      executor,
      [context = std::move(context), delay_ms,
       value]() mutable -> asio::awaitable<void> {
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(delay_ms));
        co_await timer.async_wait(asio::use_awaitable);
        auto ec = co_await context.response(value);
        CHECK_FALSE(ec);
      },
      asio::detached);
  return 0;
}

asio::awaitable<void> test_v2_multiplex(uint16_t port) {
  rpc_client client;
  auto ec = co_await client.connect("127.0.0.1", std::to_string(port));
  REQUIRE_FALSE(ec);

  auto no_serialize = co_await client.send_call_for<v2_accept_throwing>(
      std::chrono::milliseconds(0), user_codec::throwing_value{});
  auto no_serialize_result = co_await no_serialize.wait();
  CHECK(no_serialize_result.ec == rpc_errc::request_timeout);

  bool serialization_threw = false;
  try {
    auto unused = co_await client.send_call<v2_accept_throwing>(
        user_codec::throwing_value{});
    (void)unused;
  } catch (const std::runtime_error &) {
    serialization_threw = true;
  }
  CHECK(serialization_threw);

  auto slow = co_await client.send_call<v2_delay>(250, 1);
  auto fast = co_await client.send_call<v2_delay>(10, 2);

  auto start = std::chrono::steady_clock::now();
  auto fast_result = co_await fast.wait();
  auto fast_elapsed = std::chrono::steady_clock::now() - start;
  CHECK(fast_result.ec == rpc_errc::ok);
  CHECK(fast_result.value == 2);
  CHECK(fast_elapsed < std::chrono::milliseconds(150));

  auto slow_result = co_await slow.wait();
  CHECK(slow_result.ec == rpc_errc::ok);
  CHECK(slow_result.value == 1);

  auto [batch_slow, batch_fast] = client.send_calls(
      client.send_call<v2_delay>(200, 3), client.send_call<v2_delay>(5, 4));
  auto batch_fast_result = co_await batch_fast.wait();
  CHECK(batch_fast_result.ec == rpc_errc::ok);
  CHECK(batch_fast_result.value == 4);
  auto batch_slow_result = co_await batch_slow.wait();
  CHECK(batch_slow_result.ec == rpc_errc::ok);
  CHECK(batch_slow_result.value == 3);

  auto [number, text] =
      client.send_calls(client.send_call<v2_delay>(40, 7),
                        client.send_call<v2_echo_text>("multiplex"));
  auto text_result = co_await text.wait();
  CHECK(text_result.ec == rpc_errc::ok);
  CHECK(text_result.value == "multiplex");
  auto number_result = co_await number.wait();
  CHECK(number_result.ec == rpc_errc::ok);
  CHECK(number_result.value == 7);

  auto notify = co_await client.send_call<v2_notify>(9);
  auto notify_result = co_await notify.wait();
  CHECK(notify_result.ec == rpc_errc::ok);
  CHECK_FALSE(notify.valid());
  CHECK_THROWS_AS((void)notify.wait(), std::logic_error);

  auto failed = co_await client.send_call<v2_throw>();
  auto failed_result = co_await failed.wait();
  CHECK(failed_result.ec == rpc_errc::function_exception);

  auto pack_failed = co_await client.send_call<v2_manual_pack_throw>();
  auto pack_failed_result = co_await pack_failed.wait();
  CHECK(pack_failed_result.ec == rpc_errc::function_exception);

  auto bad_result = co_await client.send_call<v2_bad_result>();
  bool deserialization_threw = false;
  try {
    (void)co_await bad_result.wait();
  } catch (const std::invalid_argument &) {
    deserialization_threw = true;
  }
  CHECK(deserialization_threw);

  auto missing = co_await client.send_call<add>(1, 2);
  auto missing_result = co_await missing.wait();
  CHECK(missing_result.ec == rpc_errc::no_such_function);

  auto manual_slow = co_await client.send_call<v2_manual_delay>(100, 8);
  auto manual_fast = co_await client.send_call<v2_manual_delay>(1, 9);
  auto manual_fast_result = co_await manual_fast.wait();
  CHECK(manual_fast_result.ec == rpc_errc::ok);
  CHECK(manual_fast_result.value == 9);
  auto manual_slow_result = co_await manual_slow.wait();
  CHECK(manual_slow_result.ec == rpc_errc::ok);
  CHECK(manual_slow_result.value == 8);

  auto detached = co_await client.send_call<v2_detached_response>(10, 10);
  auto detached_result = co_await detached.wait();
  CHECK(detached_result.ec == rpc_errc::ok);
  CHECK(detached_result.value == 10);

  auto timeout_sender = co_await client.send_call_for<v2_delay>(
      std::chrono::milliseconds(20), 200, 5);
  auto timeout_result = co_await timeout_sender.wait();
  CHECK(timeout_result.ec == rpc_errc::request_timeout);

  auto immediate_timeout = co_await client.send_call_for<v2_delay>(
      std::chrono::milliseconds(0), 1, 12);
  auto immediate_timeout_result = co_await immediate_timeout.wait();
  CHECK(immediate_timeout_result.ec == rpc_errc::request_timeout);

  auto deferred_timeout = co_await client.send_call_for<v2_delay>(
      std::chrono::milliseconds(20), 200, 11);
  asio::steady_timer defer_wait(co_await asio::this_coro::executor);
  defer_wait.expires_after(std::chrono::milliseconds(50));
  co_await defer_wait.async_wait(asio::use_awaitable);
  auto deferred_timeout_result = co_await deferred_timeout.wait();
  CHECK(deferred_timeout_result.ec == rpc_errc::request_timeout);

  asio::steady_timer let_late_response_arrive(
      co_await asio::this_coro::executor);
  let_late_response_arrive.expires_after(std::chrono::milliseconds(250));
  co_await let_late_response_arrive.async_wait(asio::use_awaitable);

  auto after_timeout = co_await client.send_call<v2_delay>(1, 6);
  auto after_timeout_result = co_await after_timeout.wait();
  CHECK(after_timeout_result.ec == rpc_errc::ok);
  CHECK(after_timeout_result.value == 6);

  std::vector<async_result<int>> concurrent;
  concurrent.reserve(128);
  for (int i = 0; i < 128; ++i) {
    concurrent.push_back(co_await client.send_call<v2_delay>((i * 7) % 6, i));
  }
  for (int i = 127; i >= 0; --i) {
    auto result = co_await concurrent[static_cast<size_t>(i)].wait();
    CHECK(result.ec == rpc_errc::ok);
    CHECK(result.value == i);
  }

  std::vector<std::string> text_values;
  std::vector<async_result<std::string>> text_senders;
  for (int i = 1; i <= 32; ++i) {
    text_values.emplace_back(static_cast<size_t>(i * 17),
                             static_cast<char>('a' + i % 26));
    text_senders.push_back(
        co_await client.send_call<v2_echo_text>(text_values.back()));
  }
  for (int i = 31; i >= 0; --i) {
    auto result = co_await text_senders[static_cast<size_t>(i)].wait();
    CHECK(result.ec == rpc_errc::ok);
    CHECK(result.value == text_values[static_cast<size_t>(i)]);
  }

  auto v1_result = co_await client.call<add>(1, 2);
  CHECK(v1_result.ec == rpc_errc::protocol_mode_conflict);

  rpc_client v1_client;
  ec = co_await v1_client.connect("127.0.0.1", std::to_string(port));
  REQUIRE_FALSE(ec);
  auto v1_first = co_await v1_client.call<v2_echo_text>("v1");
  REQUIRE(v1_first.ec == rpc_errc::ok);
  CHECK(v1_first.value == "v1");
  auto v2_after_v1 = co_await v1_client.send_call<v2_echo_text>("not-sent");
  auto conflict = co_await v2_after_v1.wait();
  CHECK(conflict.ec == rpc_errc::protocol_mode_conflict);
}

TEST_CASE("test protocol v2 multiplex") {
  rpc_server server("127.0.0.1", "0");
  server.register_handler<v2_delay>();
  server.register_handler<v2_echo_text>();
  server.register_handler<v2_notify>();
  server.register_handler<v2_throw>();
  server.register_handler<v2_manual_pack_throw>();
  server.register_handler(get_func_name<v2_bad_result>(),
                          [] { return std::string("not-an-int"); });
  server.register_handler<v2_manual_delay>();
  server.register_handler<v2_detached_response>();
  REQUIRE_FALSE(server.async_start());
  REQUIRE(server.port() != 0);
  sync_wait(get_global_executor(), test_v2_multiplex(server.port()));
}

asio::awaitable<void> test_multiplex_client_limits(uint16_t port) {
  rpc_client client;
  multiplex_client_limits limits;
  limits.max_pending = 1;
  client.set_multiplex_limits(limits);
  auto ec = co_await client.connect("127.0.0.1", std::to_string(port));
  REQUIRE_FALSE(ec);

  auto first = co_await client.send_call<v2_delay>(100, 1);
  auto rejected = co_await client.send_call<v2_delay>(1, 2);
  auto rejected_result = co_await rejected.wait();
  CHECK(rejected_result.ec == rpc_errc::queue_full);
  auto first_result = co_await first.wait();
  CHECK(first_result.ec == rpc_errc::ok);
  CHECK(first_result.value == 1);
}

TEST_CASE("test protocol v2 client limits") {
  rpc_server server("127.0.0.1", "0");
  server.register_handler<v2_delay>();
  REQUIRE_FALSE(server.async_start());
  sync_wait(get_global_executor(), test_multiplex_client_limits(server.port()));
}

asio::awaitable<void> test_v2_message_limit(uint16_t port) {
  rpc_client client;
  multiplex_client_limits limits;
  limits.max_body_size = 1;
  client.set_multiplex_limits(limits);
  auto ec = co_await client.connect("127.0.0.1", std::to_string(port));
  REQUIRE_FALSE(ec);
  auto rejected = co_await client.send_call<v2_delay>(1, 2);
  auto result = co_await rejected.wait();
  CHECK(result.ec == rpc_errc::message_too_large);
}

TEST_CASE("test protocol v2 message limit") {
  rpc_server server("127.0.0.1", "0");
  server.register_handler<v2_delay>();
  REQUIRE_FALSE(server.async_start());
  sync_wait(get_global_executor(), test_v2_message_limit(server.port()));
}

asio::awaitable<void> test_multiplex_server_limits(uint16_t port) {
  rpc_client client;
  auto ec = co_await client.connect("127.0.0.1", std::to_string(port));
  REQUIRE_FALSE(ec);
  auto slow = co_await client.send_call<v2_delay>(100, 1);
  auto rejected = co_await client.send_call<v2_delay>(1, 2);
  auto rejected_result = co_await rejected.wait();
  CHECK(rejected_result.ec == rpc_errc::queue_full);
  auto slow_result = co_await slow.wait();
  CHECK(slow_result.ec == rpc_errc::ok);
  CHECK(slow_result.value == 1);
}

TEST_CASE("test protocol v2 server limits") {
  rpc_server server("127.0.0.1", "0");
  server.register_handler<v2_delay>();
  multiplex_server_limits limits;
  limits.max_handlers = 1;
  server.set_multiplex_limits(limits);
  REQUIRE_FALSE(server.async_start());
  sync_wait(get_global_executor(), test_multiplex_server_limits(server.port()));
}

asio::awaitable<void> test_v2_response_queue_limit(uint16_t port) {
  rpc_client client;
  auto ec = co_await client.connect("127.0.0.1", std::to_string(port));
  REQUIRE_FALSE(ec);
  auto sender = co_await client.send_call<v2_delay>(1, 1);
  auto result = co_await sender.wait();
  CHECK(result.ec == rpc_errc::read_error);
}

TEST_CASE("test protocol v2 response queue limit") {
  rpc_server server("127.0.0.1", "0");
  server.register_handler<v2_delay>();
  multiplex_server_limits limits;
  limits.max_response_queue = 0;
  server.set_multiplex_limits(limits);
  REQUIRE_FALSE(server.async_start());
  sync_wait(get_global_executor(), test_v2_response_queue_limit(server.port()));
}

asio::awaitable<void> test_v2_close_and_reconnect(uint16_t port) {
  rpc_client client;
  auto ec = co_await client.connect("127.0.0.1", std::to_string(port));
  REQUIRE_FALSE(ec);
  auto old = co_await client.send_call<v2_delay>(200, 1);

  ec = co_await client.connect("127.0.0.1", std::to_string(port));
  REQUIRE_FALSE(ec);
  auto current = co_await client.send_call<v2_delay>(1, 2);
  auto current_result = co_await current.wait();
  CHECK(current_result.ec == rpc_errc::ok);
  CHECK(current_result.value == 2);

  auto old_result = co_await old.wait();
  CHECK(old_result.ec == rpc_errc::socket_closed);

  auto pending = co_await client.send_call<v2_delay>(200, 3);
  auto pending_too = co_await client.send_call<v2_delay>(200, 4);
  client.close();
  auto closed_result = co_await pending.wait();
  auto closed_result_too = co_await pending_too.wait();
  CHECK(closed_result.ec == rpc_errc::socket_closed);
  CHECK(closed_result_too.ec == rpc_errc::socket_closed);
}

TEST_CASE("test protocol v2 close and reconnect isolation") {
  rpc_server server("127.0.0.1", "0");
  server.register_handler<v2_delay>();
  REQUIRE_FALSE(server.async_start());
  sync_wait(get_global_executor(), test_v2_close_and_reconnect(server.port()));
}

asio::awaitable<void> test_v2_cancellation(uint16_t port) {
  rpc_client client;
  auto ec = co_await client.connect("127.0.0.1", std::to_string(port));
  REQUIRE_FALSE(ec);
  auto sender = co_await client.send_call<v2_delay>(200, 1);

  struct cancellation_result {
    explicit cancellation_result(asio::any_io_executor executor)
        : ready(executor) {
      ready.expires_at(std::chrono::steady_clock::time_point::max());
    }
    asio::steady_timer ready;
    std::optional<call_result<int>> result;
    std::exception_ptr exception;
  };

  auto executor = co_await asio::this_coro::executor;
  auto state = std::make_shared<cancellation_result>(executor);
  asio::cancellation_signal signal;
  asio::co_spawn(executor, sender.wait(),
                 asio::bind_cancellation_slot(
                     signal.slot(), [state](std::exception_ptr exception,
                                            call_result<int> result) {
                       state->exception = std::move(exception);
                       state->result.emplace(std::move(result));
                       state->ready.cancel();
                     }));

  asio::steady_timer cancel_timer(executor);
  cancel_timer.expires_after(std::chrono::milliseconds(10));
  co_await cancel_timer.async_wait(asio::use_awaitable);
  signal.emit(asio::cancellation_type::all);

  if (!state->result) {
    auto [wait_ec] =
        co_await state->ready.async_wait(asio::as_tuple(asio::use_awaitable));
    (void)wait_ec;
  }
  CHECK_FALSE(state->exception);
  REQUIRE(state->result.has_value());
  CHECK(state->result->ec == rpc_errc::request_cancelled);

  asio::steady_timer late_timer(executor);
  late_timer.expires_after(std::chrono::milliseconds(250));
  co_await late_timer.async_wait(asio::use_awaitable);
  auto next = co_await client.send_call<v2_delay>(1, 2);
  auto next_result = co_await next.wait();
  CHECK(next_result.ec == rpc_errc::ok);
  CHECK(next_result.value == 2);
}

TEST_CASE("test protocol v2 cancellation") {
  rpc_server server("127.0.0.1", "0");
  server.register_handler<v2_delay>();
  REQUIRE_FALSE(server.async_start());
  sync_wait(get_global_executor(), test_v2_cancellation(server.port()));
}

asio::awaitable<void> test_v2_cross_ending(uint16_t port) {
  rpc_client client;
  client.enable_cross_ending(true);
  auto ec = co_await client.connect("127.0.0.1", std::to_string(port));
  REQUIRE_FALSE(ec);
  auto sender = co_await client.send_call<v2_echo_text>("cross-ending");
  auto result = co_await sender.wait();
  CHECK(result.ec == rpc_errc::ok);
  CHECK(result.value == "cross-ending");
}

TEST_CASE("test protocol v2 cross ending") {
  rpc_server server("127.0.0.1", "0");
  server.enable_cross_ending(true);
  server.register_handler<v2_echo_text>();
  REQUIRE_FALSE(server.async_start());
  sync_wait(get_global_executor(), test_v2_cross_ending(server.port()));
}

asio::awaitable<void> test_v2_write_failure(size_t successful_writes) {
  auto executor = co_await asio::this_coro::executor;
  auto transport =
      std::make_shared<failing_write_transport>(executor, successful_writes);
  auto session = std::make_shared<
      rest_rpc::detail::multiplex_client_session<failing_write_transport>>(
      transport, false, multiplex_client_limits{});

  std::vector<std::shared_ptr<rest_rpc::detail::multiplex_pending_response>>
      pending;
  for (int i = 0; i < 3; ++i) {
    rest_rpc_header header{};
    header.function_id = static_cast<uint32_t>(i + 1);
    pending.push_back(co_await asio::co_spawn(
        session->get_executor(),
        session->start_request(header, std::to_string(i),
                               std::chrono::seconds(2)),
        asio::use_awaitable));
  }

  for (auto &request : pending) {
    auto result = co_await asio::co_spawn(
        session->get_executor(),
        rest_rpc::detail::wait_multiplex_result<void>(request, session),
        asio::use_awaitable);
    CHECK(result.ec == rpc_errc::write_error);
  }
  CHECK(transport->has_closed_);
}

TEST_CASE("test protocol v2 write failure completes queued requests") {
  sync_wait(get_global_executor(), test_v2_write_failure(0));
  sync_wait(get_global_executor(), test_v2_write_failure(1));
}

DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(4007)
int main(int argc, char **argv) { return doctest::Context(argc, argv).run(); }
DOCTEST_MSVC_SUPPRESS_WARNING_POP
