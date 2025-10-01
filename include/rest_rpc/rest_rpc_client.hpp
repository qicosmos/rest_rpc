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

class rest_rpc_client {
public:
  rest_rpc_client() : socket_(get_global_executor()) {}

  auto get_executor() { return socket_.get_executor(); }

  asio::awaitable<std::error_code> connect(
      std::string_view host, std::string_view port,
      std::chrono::steady_clock::duration duration = std::chrono::seconds(5)) {
    asio::ip::tcp::resolver resolver(socket_.get_executor());

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

    if (it == endpoints.end()) {
      REST_LOG_ERROR << "resolve failed";
      co_return std::make_error_code(std::errc::bad_address);
    }

    auto endpoint = it->endpoint();
    auto conn_r = co_await (
        watchdog(duration) ||
        socket_.async_connect(endpoint, asio::as_tuple(asio::use_awaitable)));
    if (conn_r.index() == 0) {
      REST_LOG_ERROR << "connect timeout";
      co_return make_error_code(rpc_errc::connection_timeout);
    }

    auto [conn_ec] = std::get<1>(conn_r);
    if (conn_ec) {
      REST_LOG_ERROR << "connect failed";
      co_return conn_ec;
    }

    if (tcp_no_delay_) {
      socket_.set_option(asio::ip::tcp::no_delay(true));
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
  asio::awaitable<call_result<function_return_type_t<decltype(func)>>>
  call(Args &&...args) {
    return call_for<func>(std::chrono::seconds(5), std::forward<Args>(args)...);
  }

  template <auto func, typename... Args>
  asio::awaitable<call_result<function_return_type_t<decltype(func)>>>
  call_for(auto duration, Args &&...args) {
    using args_tuple = function_parameters_t<decltype(func)>;
    static_assert(std::is_constructible_v<args_tuple, Args...>,
                  "called rpc function and arguments are not match");

    using R = function_return_type_t<decltype(func)>;
    auto r = co_await (watchdog(duration) ||
                       call_impl<func>(std::forward<Args>(args)...));
    if (r.index() == 0) {
      co_return call_result<R>{rpc_errc::request_timeout};
    }
    co_return std::get<1>(r);
  }

  void enable_tcp_no_delay(bool r) { tcp_no_delay_ = r; }

  void enable_cross_ending(bool r) { cross_ending_ = r; }

private:
  template <auto func, typename... Args>
  asio::awaitable<call_result<function_return_type_t<decltype(func)>>>
  call_impl(Args &&...args) {
    rest_rpc_header header{};
    header.magic = 39;
    header.function_id = get_key<func>();
    rpc_service::msgpack_codec codec;
    auto buf = codec.pack_args(std::forward<Args>(args)...);
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

    using R = function_return_type_t<decltype(func)>;
    call_result<R> result{};
    std::error_code ec;
    size_t size;
    std::tie(ec, size) = co_await asio::async_write(
        socket_, buffers, asio::as_tuple(asio::use_awaitable));
    if (ec) {
      result.ec = rpc_errc::write_error;
      co_return result;
    }

    rest_rpc_header resp_header;
    std::tie(ec, size) = co_await asio::async_read(
        socket_, asio::buffer(&resp_header, sizeof(rest_rpc_header)),
        asio::as_tuple(asio::use_awaitable));
    if (ec) {
      result.ec = rpc_errc::write_error;
      co_return result;
    }
    if (resp_header.magic != 39) {
      result.ec = rpc_errc::protocol_error;
      co_return result;
    }

    if (cross_ending_) {
      parse_recieved(resp_header);
    }

    detail::resize(body_, resp_header.body_len);
    std::tie(ec, size) = co_await asio::async_read(
        socket_, asio::buffer(body_.data(), body_.size()),
        asio::as_tuple(asio::use_awaitable));
    if (ec) {
      REST_LOG_WARNING << "read body error: " << ec.message();
      result.ec = rpc_errc::read_error;
      co_return result;
    }
    result.ec = (rpc_errc)body_[0];
    if (resp_header.body_len > 0) {
      auto view = std::string_view(body_.data() + 1, resp_header.body_len - 1);
      REST_LOG_INFO << view;
    }
    if constexpr (!std::is_void_v<R>) {
      result.value = codec.unpack<R>(
          std::string_view(body_.data() + 1, resp_header.body_len - 1));
    }

    co_return result;
  }

  asio::awaitable<std::error_code> watchdog(auto duration) {
    asio::steady_timer timer(socket_.get_executor());
    timer.expires_after(duration);
    auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
    co_return ec;
  }

  tcp_socket socket_;
  std::string body_;
  bool tcp_no_delay_ = true;
  bool cross_ending_ = false;
};
} // namespace rest_rpc