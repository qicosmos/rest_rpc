#pragma once
#include "asio_util.hpp"
#include "codec.h"
#include "error_code.h"
#include "io_context_pool.hpp"
#include "logger.hpp"
// #include "meta_util.hpp"
#include "rest_rpc_protocol.hpp"
#include "string_resize.hpp"
#include "traits.h"
#include "use_asio.hpp"
#include "util.hpp"
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/steady_timer.hpp>
using namespace asio::experimental::awaitable_operators;

namespace rest_rpc {
template <typename R> struct call_result {
  rpc_errc ec;
  R value;
};

template <> struct call_result<void> {
  rpc_errc ec;
};

class rpc_client {
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

    rest_rpc_header header{};
    header.function_id = get_key<func>();
    using R = return_type_t<function_return_type_t<decltype(func)>>;
    auto r = co_await (watchdog(duration) ||
                       call_impl<R>(header, std::forward<Args>(args)...));
    if (r.index() == 0) {
      co_return call_result<R>{rpc_errc::request_timeout};
    }
    co_return std::get<1>(r);
  }

  template <typename R = void>
  asio::awaitable<call_result<R>> subscribe(std::string_view topic) {
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

    asio::dispatch(socket_->get_executor(),
                   [socket = socket_] { close_socket(*socket); });
  }

private:
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
      result.ec = rpc_errc::write_error;
      close_socket(*socket_);
      co_return result;
    }
    if (resp_header.magic != 39) {
      result.ec = rpc_errc::protocol_error;
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
  }

  std::shared_ptr<socket_t> socket_;
  bool tcp_no_delay_ = true;
  bool cross_ending_ = false;
  bool should_reset_ = false;
};
} // namespace rest_rpc
