#pragma once
#include "asio_util.hpp"
#include "codec.h"
#include "error_code.h"
#include "io_context_pool.hpp"
#include "logger.hpp"
// #include "meta_util.hpp"
#include "rest_rpc_protocol.hpp"
#include "rpc_client_multiplex_session.hpp"
#include "string_resize.hpp"
#include "traits.h"
#include "use_asio.hpp"
#include "util.hpp"
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/steady_timer.hpp>
#include <optional>
#include <stdexcept>
using namespace asio::experimental::awaitable_operators;

namespace rest_rpc {
template <typename R> class async_result;

template <typename R> struct call_result {
  rpc_errc ec;
  R value;
};

template <> struct call_result<void> {
  rpc_errc ec;
};

class rpc_client {
  struct socket_t;
  using multiplex_session_t = detail::multiplex_client_session<socket_t>;

public:
  rpc_client() : socket_(std::make_shared<socket_t>(get_global_executor())) {}
  ~rpc_client() { close(); }

  auto get_executor() { return socket_->get_executor(); }

  asio::awaitable<std::error_code> connect(
      std::string_view host, std::string_view port,
      std::chrono::steady_clock::duration duration = std::chrono::seconds(5)) {
    if (should_reset_ || mode_ != protocol_mode::unset) {
      reset();
    } else {
      should_reset_ = true;
    }

    asio::ip::tcp::resolver resolver(socket_->get_executor());

    auto r = co_await (watchdog(duration) ||
                       resolver.async_resolve(
                           host, port, asio::as_tuple(asio::use_awaitable)));
    if (r.index() == 0) {
      REST_LOG_ERROR << "resolve timeout";
      co_return make_error_code(rpc_errc::resolve_timeout);
    }

    auto [ec, endpoints] = std::get<1>(r);
    if (ec) {
      REST_LOG_ERROR << "resolve failed";
      co_return ec;
    }
    auto it = endpoints.begin();

    auto endpoint = it->endpoint();
    auto conn_r = co_await (watchdog(duration) ||
                            socket_->impl_.async_connect(
                                endpoint, asio::as_tuple(asio::use_awaitable)));
    if (conn_r.index() == 0) {
      REST_LOG_ERROR << "connect timeout";
      co_return make_error_code(rpc_errc::connection_timeout);
    }

    auto [conn_ec] = std::get<1>(conn_r);
    if (conn_ec) {
      REST_LOG_ERROR << "connect failed";
      co_return conn_ec;
    }

    socket_->has_closed_ = false;

    if (tcp_no_delay_) {
      socket_->impl_.set_option(asio::ip::tcp::no_delay(true));
    }

    co_return std::error_code{};
  }

  asio::awaitable<std::error_code> connect(
      std::string_view address,
      std::chrono::steady_clock::duration duration = std::chrono::seconds(5)) {
    std::string_view host;
    std::string_view port;
    size_t pos = address.find(':');
    if (pos != std::string::npos) {
      host = address.substr(0, pos);
      port = address.substr(pos + 1);
    }

    return connect(host, port, duration);
  }

  template <auto func, typename... Args>
  asio::awaitable<
      call_result<return_type_t<function_return_type_t<decltype(func)>>>>
  call(Args &&...args) {
    return call_for<func>(std::chrono::seconds(5), std::forward<Args>(args)...);
  }

  template <auto func, typename... Args>
  asio::awaitable<
      call_result<return_type_t<function_return_type_t<decltype(func)>>>>
  call_for(auto duration, Args &&...args) {
    using args_tuple = function_parameters_t<decltype(func)>;
    static_assert(std::is_constructible_v<args_tuple, Args...>,
                  "called rpc function and arguments are not match");

    using R = return_type_t<function_return_type_t<decltype(func)>>;
    using duration_type = std::remove_cvref_t<decltype(duration)>;
    if (duration <= duration_type::zero()) {
      co_return call_result<R>{rpc_errc::request_timeout};
    }
    if (!select_mode(protocol_mode::v1)) {
      call_result<R> result{};
      result.ec = rpc_errc::protocol_mode_conflict;
      co_return result;
    }

    rest_rpc_header header{};
    header.function_id = get_key<func>();
    auto r = co_await (watchdog(duration) ||
                       call_impl<R>(header, std::forward<Args>(args)...));
    if (r.index() == 0) {
      co_return call_result<R>{rpc_errc::request_timeout};
    }
    co_return std::get<1>(r);
  }

  template <auto func, typename... Args>
  asio::awaitable<
      async_result<return_type_t<function_return_type_t<decltype(func)>>>>
  send_call(Args &&...args);

  template <auto func, typename Rep, typename Period, typename... Args>
  asio::awaitable<
      async_result<return_type_t<function_return_type_t<decltype(func)>>>>
  send_call_for(std::chrono::duration<Rep, Period> timeout, Args &&...args);

  template <typename... Calls> auto send_calls(Calls &&...calls);

  void set_multiplex_limits(multiplex_client_limits limits);

  template <typename R = void>
  asio::awaitable<call_result<R>> subscribe(std::string_view topic) {
    if (!select_mode(protocol_mode::v1)) {
      call_result<R> result{};
      result.ec = rpc_errc::protocol_mode_conflict;
      co_return result;
    }

    uint32_t topic_id =
        MD5::MD5Hash32(topic.data(), (uint32_t)topic.size()); // topic id
    bool b = false;
    call_result<R> ret{};
    auto it = socket_->sub_ops_.find(topic_id);
    if (it == socket_->sub_ops_.end()) {
      rest_rpc_header header{};
      header.msg_type = 1; // pub/sub

      header.function_id = topic_id;
      auto [it, r] = socket_->sub_ops_.emplace(topic_id, sub_operation{});

      std::tie(b, ret) = co_await (
          asio::async_compose<decltype(asio::use_awaitable), void(bool)>(
              std::ref(it->second), asio::use_awaitable) &&
          call_impl<R>(header));
    } else {
      std::tie(b, ret) = co_await (
          asio::async_compose<decltype(asio::use_awaitable), void(bool)>(
              std::ref(it->second), asio::use_awaitable) &&
          wait_response<R>());
    }

    co_return std::move(ret);
  }

  void enable_tcp_no_delay(bool r) { tcp_no_delay_ = r; }

  void enable_cross_ending(bool r) { cross_ending_ = r; }
  bool has_closed() const { return socket_->has_closed_; }

  void close() {
    if (socket_ == nullptr || socket_->has_closed_)
      return;

    if (multiplex_session_) {
      multiplex_session_->request_shutdown();
      return;
    }

    asio::dispatch(socket_->get_executor(),
                   [socket = socket_] { close_socket(*socket); });
  }

private:
  enum class protocol_mode { unset, v1, multiplex };

  template <auto func, typename Tuple>
  asio::awaitable<
      async_result<return_type_t<function_return_type_t<decltype(func)>>>>
  send_call_owned(std::chrono::steady_clock::duration timeout,
                  Tuple owned_args);

  template <auto func, typename R>
  asio::awaitable<async_result<R>>
  start_multiplex_request(std::chrono::steady_clock::duration timeout,
                          std::string body);

  template <typename Outer> auto start_multiplex_call(Outer &&outer);

  bool select_mode(protocol_mode requested) {
    if (mode_ == protocol_mode::unset) {
      mode_ = requested;
      return true;
    }
    return mode_ == requested;
  }

  template <typename... Args> auto get_buffer(Args &&...args) {
    if constexpr (sizeof...(Args) == 0) {
      return rpc_codec::pack_args();
    } else if constexpr (sizeof...(Args) > 1) {
      return rpc_codec::pack_args(
          std::forward_as_tuple(std::forward<Args>(args)...));
    } else {
      if constexpr (util::is_basic_v<Args...>) {
        return rpc_codec::pack_args(std::forward<Args>(args)...);
      } else {
        return rpc_codec::pack_args(
            std::forward_as_tuple(std::forward<Args>(args)...));
      }
    }
  }

  template <typename R, typename... Args>
  asio::awaitable<call_result<R>> call_impl(rest_rpc_header &header,
                                            Args &&...args) {
    auto buf = get_buffer(std::forward<Args>(args)...);
    header.body_len = buf.size();
    if (cross_ending_) {
      prepare_for_send(header);
    }

    std::vector<asio::const_buffer> buffers;
    buffers.reserve(2);
    buffers.push_back(asio::buffer(&header, sizeof(rest_rpc_header)));
    if constexpr (sizeof...(Args) > 0) {
      buffers.push_back(asio::buffer(buf.data(), buf.size()));
    }

    call_result<R> result{};
    std::error_code ec;
    size_t size;
    std::tie(ec, size) = co_await asio::async_write(
        socket_->impl_, buffers, asio::as_tuple(asio::use_awaitable));
    if (ec) {
      result.ec = rpc_errc::write_error;
      close_socket(*socket_);
      co_return result;
    }

    co_return co_await wait_response<R>();
  }

  template <typename R> asio::awaitable<call_result<R>> wait_response() {
    call_result<R> result{};
    std::error_code ec;
    size_t size;
    rest_rpc_header resp_header;
    std::tie(ec, size) = co_await asio::async_read(
        socket_->impl_, asio::buffer(&resp_header, sizeof(rest_rpc_header)),
        asio::as_tuple(asio::use_awaitable));
    if (ec) {
      result.ec = rpc_errc::read_error;
      close_socket(*socket_);
      comple_all();
      co_return result;
    }
    if (resp_header.magic != 39) {
      result.ec = rpc_errc::protocol_error;
      comple_all();
      co_return result;
    }

    if (cross_ending_) {
      parse_recieved(resp_header);
    }

    detail::resize(socket_->body_, resp_header.body_len);
    std::tie(ec, size) = co_await asio::async_read(
        socket_->impl_,
        asio::buffer(socket_->body_.data(), socket_->body_.size()),
        asio::as_tuple(asio::use_awaitable));
    if (ec) {
      REST_LOG_WARNING << "read body error: " << ec.message();
      result.ec = rpc_errc::read_error;
      close_socket(*socket_);
      comple_all();
      co_return result;
    }
    result.ec = (rpc_errc)socket_->body_[0];
    if constexpr (!std::is_void_v<R>) {
      result.value = rpc_codec::unpack<R>(std::string_view(
          socket_->body_.data() + 1, resp_header.body_len - 1));
    }

    if (resp_header.msg_type == 1) { // pubsub
      if (auto it = socket_->sub_ops_.find(resp_header.function_id);
          it != socket_->sub_ops_.end()) {
        it->second.complete(true);
      }
    }
    co_return std::move(result);
  }

  void comple_all() {
    for (auto &pair : socket_->sub_ops_) {
      pair.second.complete(false);
    }
  }

  asio::awaitable<std::error_code> watchdog(auto duration) {
    asio::steady_timer timer(socket_->get_executor());
    timer.expires_after(duration);
    auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
    if (!ec) {
      close_socket(*socket_);
    }
    co_return ec;
  }

  class sub_operation {
  public:
    template <typename Self> void operator()(Self &&self) {
      using SelfType = std::decay_t<Self>;
      auto shared_self = std::make_shared<SelfType>(std::move(self));

      complete_handler_ = [shared_self](bool r) mutable {
        shared_self->complete(r);
      };
    }

    void complete(bool r) { complete_handler_(r); }

  private:
    std::function<void(bool)> complete_handler_;
  };

  struct socket_t {
    socket_t(auto executor) : impl_(executor) {}
    asio::any_io_executor get_executor() { return impl_.get_executor(); }
    asio::ip::tcp::socket impl_;
    std::atomic<bool> has_closed_ = true;
    std::string body_;
    std::unordered_map<uint32_t, sub_operation> sub_ops_;
  };

  inline static void close_socket(socket_t &socket) {
    std::error_code ec;
    socket.impl_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket.impl_.close(ec);
    socket.has_closed_ = true;
  }

  void reset() {
    auto executor = socket_->get_executor();
    if (multiplex_session_) {
      multiplex_session_->request_shutdown();
      socket_ = std::make_shared<socket_t>(executor);
      multiplex_session_.reset();
      mode_ = protocol_mode::unset;
      return;
    }

    if (!has_closed()) {
      close_socket(*socket_);
    }

    socket_->impl_ = asio::ip::tcp::socket{executor};
    if (!socket_->impl_.is_open()) {
      std::error_code ec;
      socket_->impl_.open(asio::ip::tcp::v4(), ec);
      if (ec) {
        REST_LOG_WARNING << "client reset socket failed, reason: "
                         << ec.message();
        return;
      }
    }
    mode_ = protocol_mode::unset;
  }

  std::shared_ptr<socket_t> socket_;
  std::shared_ptr<multiplex_session_t> multiplex_session_;
  multiplex_client_limits multiplex_limits_;
  protocol_mode mode_ = protocol_mode::unset;
  bool tcp_no_delay_ = true;
  bool cross_ending_ = false;
  bool should_reset_ = false;
};

class rpc_context;

template <typename R> class async_result {
public:
  using value_type = call_result<R>;

  async_result() = default;
  explicit async_result(asio::awaitable<value_type> operation)
      : operation_(std::move(operation)) {}

  async_result(async_result &&) noexcept = default;
  async_result &operator=(async_result &&) noexcept = default;
  async_result(const async_result &) = delete;
  async_result &operator=(const async_result &) = delete;

  [[nodiscard]] asio::awaitable<value_type> wait() {
    if (!operation_.valid()) {
      throw std::logic_error("async_result can only be awaited once");
    }
    return std::move(operation_);
  }

  bool valid() const noexcept { return operation_.valid(); }

private:
  asio::awaitable<value_type> operation_;
};

namespace detail {

template <typename Tuple> struct multiplex_call_parameters {
  using type = Tuple;
};

template <typename First, typename... Args>
struct multiplex_call_parameters<std::tuple<First, Args...>> {
  using type = std::conditional_t<
      std::is_same_v<std::remove_cvref_t<First>, rpc_context>,
      std::tuple<Args...>, std::tuple<First, Args...>>;
};

template <typename Tuple>
using multiplex_call_parameters_t =
    typename multiplex_call_parameters<Tuple>::type;

template <typename T> auto own_multiplex_arg(T &&value) {
  if constexpr (util::CharArray<T> ||
                std::is_same_v<std::remove_cvref_t<T>, std::string_view>) {
    return std::string(std::string_view(value));
  } else {
    return std::decay_t<T>(std::forward<T>(value));
  }
}

template <typename R> call_result<R> make_multiplex_result(rpc_errc ec) {
  call_result<R> result{};
  result.ec = ec;
  return result;
}

template <typename R>
asio::awaitable<call_result<R>> make_ready_multiplex_result(rpc_errc ec) {
  co_return make_multiplex_result<R>(ec);
}

template <typename R>
asio::awaitable<async_result<R>> make_ready_multiplex_sender(rpc_errc ec) {
  co_return async_result<R>{make_ready_multiplex_result<R>(ec)};
}

template <typename R>
asio::awaitable<call_result<R>>
wait_multiplex_result(std::shared_ptr<multiplex_pending_response> pending) {
  if (!pending->completed) {
    auto [wait_ec] =
        co_await pending->ready.async_wait(asio::as_tuple(asio::use_awaitable));
    if (!pending->completed) {
      // Ending this wait does not end the RPC; its deadline remains active.
      throw std::system_error(wait_ec);
    }
  }

  auto result = make_multiplex_result<R>(pending->ec);
  if constexpr (!std::is_void_v<R>) {
    if (result.ec == rpc_errc::ok) {
      result.value = rpc_codec::unpack<R>(pending->body);
    }
  }
  co_return result;
}

template <typename Inner> struct multiplex_start_state {
  explicit multiplex_start_state(asio::any_io_executor executor)
      : ready(executor) {
    ready.expires_at(std::chrono::steady_clock::time_point::max());
  }

  asio::steady_timer ready;
  std::optional<Inner> sender;
  std::exception_ptr exception;
  bool completed = false;
};

template <typename Inner>
asio::awaitable<typename Inner::value_type>
await_multiplex_start(std::shared_ptr<multiplex_start_state<Inner>> state) {
  if (!state->completed) {
    auto [ec] =
        co_await state->ready.async_wait(asio::as_tuple(asio::use_awaitable));
    if (!state->completed) {
      throw std::system_error(ec);
    }
  }
  if (state->exception) {
    std::rethrow_exception(state->exception);
  }
  co_return co_await state->sender->wait();
}

template <typename Inner>
Inner make_multiplex_start_result(
    std::shared_ptr<multiplex_start_state<Inner>> state) {
  auto executor = state->ready.get_executor();
  return Inner{asio::co_spawn(executor,
                              await_multiplex_start<Inner>(std::move(state)),
                              asio::use_awaitable)};
}

} // namespace detail

template <auto func, typename... Args>
asio::awaitable<
    async_result<return_type_t<function_return_type_t<decltype(func)>>>>
rpc_client::send_call(Args &&...args) {
  return send_call_for<func>(std::chrono::seconds(5),
                             std::forward<Args>(args)...);
}

template <auto func, typename Rep, typename Period, typename... Args>
asio::awaitable<
    async_result<return_type_t<function_return_type_t<decltype(func)>>>>
rpc_client::send_call_for(std::chrono::duration<Rep, Period> timeout,
                          Args &&...args) {
  using R = return_type_t<function_return_type_t<decltype(func)>>;
  using args_tuple = function_parameters_t<decltype(func)>;
  using call_args = detail::multiplex_call_parameters_t<args_tuple>;
  static_assert(std::is_constructible_v<call_args, Args...>,
                "called rpc function and arguments are not match");
  const auto steady_timeout =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout);
  if (steady_timeout <= std::chrono::steady_clock::duration::zero()) {
    return detail::make_ready_multiplex_sender<R>(rpc_errc::request_timeout);
  }
  auto owned_args =
      std::tuple{detail::own_multiplex_arg(std::forward<Args>(args))...};
  return send_call_owned<func>(steady_timeout, std::move(owned_args));
}

template <typename... Calls> auto rpc_client::send_calls(Calls &&...calls) {
  return std::tuple{start_multiplex_call(std::forward<Calls>(calls))...};
}

inline void rpc_client::set_multiplex_limits(multiplex_client_limits limits) {
  multiplex_limits_ = limits;
  if (multiplex_session_) {
    multiplex_session_->set_limits(limits);
  }
}

template <auto func, typename Tuple>
asio::awaitable<
    async_result<return_type_t<function_return_type_t<decltype(func)>>>>
rpc_client::send_call_owned(std::chrono::steady_clock::duration timeout,
                            Tuple owned_args) {
  using R = return_type_t<function_return_type_t<decltype(func)>>;

  auto buffer = std::apply(
      [this](auto &...args) { return get_buffer(std::move(args)...); },
      owned_args);
  std::string body;
  if (!buffer.empty()) {
    body.assign(buffer.data(), buffer.size());
  }

  co_return co_await asio::co_spawn(
      socket_->get_executor(),
      start_multiplex_request<func, R>(timeout, std::move(body)),
      asio::use_awaitable);
}

template <auto func, typename R>
asio::awaitable<async_result<R>>
rpc_client::start_multiplex_request(std::chrono::steady_clock::duration timeout,
                                    std::string body) {
  if (!select_mode(protocol_mode::multiplex)) {
    co_return async_result<R>{detail::make_ready_multiplex_result<R>(
        rpc_errc::protocol_mode_conflict)};
  }
  if (!multiplex_session_) {
    multiplex_session_ = std::make_shared<multiplex_session_t>(
        socket_, cross_ending_, multiplex_limits_);
  }

  rest_rpc_header header{};
  header.version = REST_RPC_PROTOCOL_V2;
  header.function_id = get_key<func>();

  auto pending = co_await asio::co_spawn(
      multiplex_session_->get_executor(),
      multiplex_session_->start_request(header, std::move(body), timeout),
      asio::use_awaitable);
  co_return async_result<R>{
      asio::co_spawn(multiplex_session_->get_executor(),
                     detail::wait_multiplex_result<R>(std::move(pending)),
                     asio::use_awaitable)};
}

template <typename Outer> auto rpc_client::start_multiplex_call(Outer &&outer) {
  using outer_t = std::remove_cvref_t<Outer>;
  using inner_t = typename outer_t::value_type;
  auto state = std::make_shared<detail::multiplex_start_state<inner_t>>(
      socket_->get_executor());
  async_start(socket_->get_executor(), std::forward<Outer>(outer),
              [state](std::exception_ptr exception, inner_t sender) {
                state->exception = std::move(exception);
                state->sender.emplace(std::move(sender));
                state->completed = true;
                state->ready.cancel();
              });
  return detail::make_multiplex_start_result<inner_t>(std::move(state));
}

} // namespace rest_rpc
