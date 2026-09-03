#pragma once
#include "asio_util.hpp"
#include "codec.h"
#include "error_code.h"
#include "io_context_pool.hpp"
#include "logger.hpp"
#include "rest_rpc_protocol.hpp"
#include "string_resize.hpp"
#include "traits.h"
#include "use_asio.hpp"
#include "util.hpp"
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/steady_timer.hpp>
#include <cstring>
#include <deque>
#include <unordered_set>

using namespace asio::experimental::awaitable_operators;

namespace rest_rpc {
template <typename R> struct call_result {
  rpc_errc ec;
  R value;
};

template <> struct call_result<void> { rpc_errc ec; };

class rpc_client {
  enum class peer_mode { unknown, multiplex, legacy };

  struct raw_response {
    rpc_errc transport_ec = rpc_errc::ok;
    std::string body;
  };

  struct pending_response {
    pending_response(asio::any_io_executor executor,
                     std::chrono::steady_clock::duration timeout)
        : timer_(executor) {
      timer_.expires_after(timeout);
    }

    void complete(raw_response response) {
      if (ready_) {
        return;
      }
      response_ = std::move(response);
      ready_ = true;
      timer_.cancel();
    }

    asio::steady_timer timer_;
    raw_response response_;
    bool ready_ = false;
  };

  struct subscription_state {
    explicit subscription_state(asio::any_io_executor executor)
        : notify_(executor, (std::chrono::steady_clock::time_point::max)()) {}

    void push(raw_response response) {
      responses_.push_back(std::move(response));
      notify_.cancel();
    }

    void fail(rpc_errc ec) {
      failure_ = ec;
      failed_ = true;
      notify_.cancel();
    }

    asio::steady_timer notify_;
    std::deque<raw_response> responses_;
    rpc_errc failure_ = rpc_errc::ok;
    bool failed_ = false;
  };

  struct write_request {
    uint64_t seq_num = 0;
    bool check_pending = false;
    std::string data;
  };

  struct socket_t {
    socket_t(auto executor) : impl_(executor) {}
    asio::any_io_executor get_executor() { return impl_.get_executor(); }

    asio::ip::tcp::socket impl_;
    std::atomic<bool> has_closed_ = true;
    uint64_t generation_ = 0;
    uint64_t next_seq_num_ = 0;
    bool writing_ = false;
    bool legacy_seq_zero_safe_ = true;
    peer_mode peer_mode_ = peer_mode::unknown;
    std::deque<write_request> write_queue_;
    std::unordered_map<uint64_t, std::shared_ptr<pending_response>> pending_;
    std::unordered_set<uint64_t> timed_out_;
    std::unordered_map<uint32_t, std::shared_ptr<subscription_state>> sub_ops_;
  };

public:
  rpc_client() : socket_(std::make_shared<socket_t>(get_global_executor())) {}
  ~rpc_client() { close(); }

  auto get_executor() { return socket_->get_executor(); }

  asio::awaitable<std::error_code> connect(
      std::string_view host, std::string_view port,
      std::chrono::steady_clock::duration duration = std::chrono::seconds(5)) {
    if (should_reset_) {
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
    if (ec || endpoints.empty()) {
      REST_LOG_ERROR << "resolve failed";
      co_return ec ? ec : std::make_error_code(std::errc::host_unreachable);
    }

    auto conn_r = co_await (
        watchdog(duration) ||
        socket_->impl_.async_connect(endpoints.begin()->endpoint(),
                                     asio::as_tuple(asio::use_awaitable)));
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

    asio::co_spawn(socket_->get_executor(),
                   read_loop(socket_, cross_ending_, socket_->generation_),
                   asio::detached);
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

    rest_rpc_header header{};
    header.function_id = get_key<func>();
    using R = return_type_t<function_return_type_t<decltype(func)>>;
    auto timeout =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            duration);
    if (socket_->has_closed_) {
      return immediate_result<R>(rpc_errc::socket_closed);
    }
    if (timeout <= std::chrono::steady_clock::duration::zero()) {
      return immediate_result<R>(rpc_errc::request_timeout);
    }

    auto buf = get_buffer(std::forward<Args>(args)...);
    std::string request_body;
    if (!buf.empty()) {
      request_body.assign(buf.data(), buf.size());
    }
    return call_impl<R>(socket_, header, timeout, std::move(request_body),
                        cross_ending_);
  }

  template <typename R = void>
  asio::awaitable<call_result<R>> subscribe(std::string_view topic) {
    return subscribe_impl<R>(socket_, std::string(topic), cross_ending_);
  }

  void enable_tcp_no_delay(bool r) { tcp_no_delay_ = r; }
  void enable_cross_ending(bool r) { cross_ending_ = r; }
  bool has_closed() const { return socket_->has_closed_; }

  void close() {
    if (socket_ == nullptr || socket_->has_closed_) {
      return;
    }
    asio::dispatch(socket_->get_executor(), [socket = socket_] {
      fail_all(*socket, rpc_errc::socket_closed);
      close_socket(*socket);
    });
  }

private:
  template <typename R>
  static asio::awaitable<call_result<R>>
  subscribe_impl(std::shared_ptr<socket_t> socket, std::string topic,
                 bool cross_ending) {
    if (socket->has_closed_) {
      co_return call_result<R>{rpc_errc::socket_closed};
    }
    uint32_t topic_id =
        MD5::MD5Hash32(topic.data(), static_cast<uint32_t>(topic.size()));
    auto it = socket->sub_ops_.find(topic_id);
    if (it == socket->sub_ops_.end()) {
      auto state = std::make_shared<subscription_state>(socket->get_executor());
      it = socket->sub_ops_.emplace(topic_id, state).first;

      rest_rpc_header header{};
      header.msg_type = 1;
      header.function_id = topic_id;
      queue_write(socket, write_request{0, false,
                                        make_frame(header, {}, cross_ending)});
    }
    co_return co_await wait_subscription<R>(it->second);
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

  template <typename R>
  static asio::awaitable<call_result<R>> immediate_result(rpc_errc ec) {
    co_return call_result<R>{ec};
  }

  template <typename R>
  static asio::awaitable<call_result<R>>
  call_impl(std::shared_ptr<socket_t> socket, rest_rpc_header header,
            std::chrono::steady_clock::duration duration,
            std::string request_body, bool cross_ending) {
    call_result<R> result{};
    if (socket->has_closed_) {
      result.ec = rpc_errc::socket_closed;
      co_return result;
    }
    if (duration <= std::chrono::steady_clock::duration::zero()) {
      result.ec = rpc_errc::request_timeout;
      co_return result;
    }
    if (socket->peer_mode_ == peer_mode::legacy && !socket->pending_.empty()) {
      result.ec = rpc_errc::protocol_error;
      co_return result;
    }

    do {
      header.seq_num = ++socket->next_seq_num_;
    } while (header.seq_num == 0 || socket->pending_.contains(header.seq_num) ||
             socket->timed_out_.contains(header.seq_num));
    header.body_len = request_body.size();

    auto pending =
        std::make_shared<pending_response>(socket->get_executor(), duration);
    socket->pending_.emplace(header.seq_num, pending);
    queue_write(socket,
                write_request{header.seq_num, true,
                              make_frame(header, request_body, cross_ending)});

    auto [timer_ec] = co_await pending->timer_.async_wait(
        asio::as_tuple(asio::use_awaitable));
    if (!pending->ready_) {
      socket->pending_.erase(header.seq_num);
      if (socket->peer_mode_ == peer_mode::legacy) {
        fail_all(*socket, rpc_errc::socket_closed);
        close_socket(*socket);
      } else if (socket->timed_out_.size() >= max_abandoned_requests_) {
        fail_all(*socket, rpc_errc::protocol_error);
        close_socket(*socket);
      } else {
        socket->timed_out_.insert(header.seq_num);
      }
      socket->legacy_seq_zero_safe_ = false;
      result.ec = rpc_errc::request_timeout;
      co_return result;
    }

    if (pending->response_.transport_ec != rpc_errc::ok) {
      result.ec = pending->response_.transport_ec;
      co_return result;
    }

    auto &body = pending->response_.body;
    result.ec = static_cast<rpc_errc>(static_cast<int8_t>(body[0]));
    if constexpr (!std::is_void_v<R>) {
      if (result.ec == rpc_errc::ok) {
        result.value = rpc_codec::unpack<R>(
            std::string_view(body.data() + 1, body.size() - 1));
      }
    }
    co_return result;
  }

  template <typename R>
  static asio::awaitable<call_result<R>>
  wait_subscription(std::shared_ptr<subscription_state> state) {
    while (state->responses_.empty() && !state->failed_) {
      co_await state->notify_.async_wait(asio::as_tuple(asio::use_awaitable));
    }
    if (state->responses_.empty()) {
      co_return call_result<R>{state->failure_};
    }

    auto response = std::move(state->responses_.front());
    state->responses_.pop_front();
    call_result<R> result{};
    result.ec = static_cast<rpc_errc>(static_cast<int8_t>(response.body[0]));
    if constexpr (!std::is_void_v<R>) {
      if (result.ec == rpc_errc::ok) {
        result.value = rpc_codec::unpack<R>(std::string_view(
            response.body.data() + 1, response.body.size() - 1));
      }
    }
    co_return result;
  }

  static std::string make_frame(rest_rpc_header header, std::string_view body,
                                bool cross_ending) {
    if (cross_ending) {
      prepare_for_send(header);
    }
    std::string frame(sizeof(rest_rpc_header) + body.size(), '\0');
    std::memcpy(frame.data(), &header, sizeof(rest_rpc_header));
    if (!body.empty()) {
      std::memcpy(frame.data() + sizeof(rest_rpc_header), body.data(),
                  body.size());
    }
    return frame;
  }

  static void queue_write(const std::shared_ptr<socket_t> &socket,
                          write_request request) {
    socket->write_queue_.push_back(std::move(request));
    if (!socket->writing_) {
      socket->writing_ = true;
      asio::co_spawn(socket->get_executor(),
                     write_loop(socket, socket->generation_), asio::detached);
    }
  }

  static asio::awaitable<void> write_loop(std::shared_ptr<socket_t> socket,
                                          uint64_t generation) {
    while (generation == socket->generation_ && !socket->write_queue_.empty()) {
      auto request = std::move(socket->write_queue_.front());
      socket->write_queue_.pop_front();
      if (request.check_pending &&
          !socket->pending_.contains(request.seq_num)) {
        continue;
      }

      auto [ec, size] =
          co_await asio::async_write(socket->impl_, asio::buffer(request.data),
                                     asio::as_tuple(asio::use_awaitable));
      if (generation != socket->generation_) {
        co_return;
      }
      if (ec) {
        fail_all(*socket, rpc_errc::write_error);
        close_socket(*socket);
        co_return;
      }
    }
    if (generation == socket->generation_) {
      socket->writing_ = false;
    }
  }

  static asio::awaitable<void> read_loop(std::shared_ptr<socket_t> socket,
                                         bool cross_ending,
                                         uint64_t generation) {
    while (generation == socket->generation_) {
      rest_rpc_header header{};
      auto [ec, size] = co_await asio::async_read(
          socket->impl_, asio::buffer(&header, sizeof(header)),
          asio::as_tuple(asio::use_awaitable));
      if (generation != socket->generation_) {
        co_return;
      }
      if (ec) {
        fail_all(*socket, rpc_errc::read_error);
        close_socket(*socket);
        co_return;
      }
      if (header.magic != REST_MAGIC_NUM) {
        fail_all(*socket, rpc_errc::protocol_error);
        close_socket(*socket);
        co_return;
      }
      if (cross_ending) {
        parse_recieved(header);
      }
      if (header.msg_type > 1 || header.attach_length != 0 ||
          header.body_len == 0 || header.body_len > std::string{}.max_size()) {
        fail_all(*socket, rpc_errc::protocol_error);
        close_socket(*socket);
        co_return;
      }

      std::string body;
      try {
        detail::resize(body, static_cast<size_t>(header.body_len));
      } catch (...) {
        fail_all(*socket, rpc_errc::protocol_error);
        close_socket(*socket);
        co_return;
      }
      if (!body.empty()) {
        std::tie(ec, size) =
            co_await asio::async_read(socket->impl_, asio::buffer(body),
                                      asio::as_tuple(asio::use_awaitable));
        if (generation != socket->generation_) {
          co_return;
        }
        if (ec) {
          fail_all(*socket, rpc_errc::read_error);
          close_socket(*socket);
          co_return;
        }
      }
      if (body.empty()) {
        fail_all(*socket, rpc_errc::protocol_error);
        close_socket(*socket);
        co_return;
      }

      raw_response response{rpc_errc::ok, std::move(body)};
      if (header.msg_type == 1) {
        if (auto it = socket->sub_ops_.find(header.function_id);
            it != socket->sub_ops_.end()) {
          it->second->push(std::move(response));
        }
        continue;
      }

      if (auto it = socket->pending_.find(header.seq_num);
          it != socket->pending_.end()) {
        if (header.seq_num != 0) {
          if (socket->peer_mode_ == peer_mode::legacy) {
            fail_all(*socket, rpc_errc::protocol_error);
            close_socket(*socket);
            co_return;
          }
          socket->peer_mode_ = peer_mode::multiplex;
        }
        auto pending = std::move(it->second);
        socket->pending_.erase(it);
        pending->complete(std::move(response));
      } else if (header.seq_num == 0 &&
                 socket->peer_mode_ != peer_mode::multiplex &&
                 socket->legacy_seq_zero_safe_ &&
                 socket->pending_.size() == 1) {
        socket->peer_mode_ = peer_mode::legacy;
        auto it = socket->pending_.begin();
        auto pending = std::move(it->second);
        socket->pending_.erase(it);
        pending->complete(std::move(response));
      } else if (socket->timed_out_.erase(header.seq_num) == 0) {
        fail_all(*socket, rpc_errc::protocol_error);
        close_socket(*socket);
        co_return;
      } else if (header.seq_num != 0 &&
                 socket->peer_mode_ == peer_mode::unknown) {
        socket->peer_mode_ = peer_mode::multiplex;
      }
    }
  }

  static void fail_all(socket_t &socket, rpc_errc ec) {
    auto pending = std::move(socket.pending_);
    socket.pending_.clear();
    for (auto &[seq, operation] : pending) {
      operation->complete(raw_response{ec});
    }
    for (auto &[topic, subscription] : socket.sub_ops_) {
      subscription->fail(ec);
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

  inline static void close_socket(socket_t &socket) {
    std::error_code ec;
    socket.impl_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket.impl_.close(ec);
    socket.has_closed_ = true;
  }

  void reset() {
    auto executor = socket_->get_executor();
    ++socket_->generation_;
    fail_all(*socket_, rpc_errc::socket_closed);
    socket_->sub_ops_.clear();
    socket_->write_queue_.clear();
    socket_->timed_out_.clear();
    socket_->next_seq_num_ = 0;
    socket_->legacy_seq_zero_safe_ = true;
    socket_->peer_mode_ = peer_mode::unknown;
    socket_->writing_ = false;
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
      }
    }
  }

  std::shared_ptr<socket_t> socket_;
  bool tcp_no_delay_ = true;
  bool cross_ending_ = false;
  bool should_reset_ = false;
  inline static constexpr size_t max_abandoned_requests_ = 4096;
};
} // namespace rest_rpc
