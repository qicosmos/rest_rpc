#pragma once
#include "logger.hpp"
#include "rest_rpc_protocol.hpp"
#include "rpc_router.hpp"
#include "string_resize.hpp"
#include "use_asio.hpp"

namespace rest_rpc {
class rpc_connection;

class rpc_context {
public:
  static auto &context() {
    thread_local rpc_context instance;
    return instance;
  }

  rpc_context(rpc_context &&o) : conn_(std::move(o.conn_)), delay_(o.delay_) {}
  rpc_context(const rpc_context &o) = delete;

  void set_connection(std::shared_ptr<rpc_connection> conn) { conn_ = conn; }

  bool delay() { return delay_; }

  void set_delay(bool r) { context().delay_ = r; }

  auto get_executor();

  template <auto func, typename... Args>
  asio::awaitable<std::error_code> response(Args &&...args);

  template <auto func, typename... Args>
  std::error_code sync_response(Args &&...args);

private:
  rpc_context() = default;
  std::shared_ptr<rpc_connection> conn_;
  bool delay_ = false;
  bool has_response_ = false;
};

class rpc_connection : public std::enable_shared_from_this<rpc_connection> {
public:
  rpc_connection(tcp_socket socket, uint64_t conn_id, rpc_router &router,
                 bool &cross_ending)
      : socket_(std::move(socket)), conn_id_(conn_id), router_(router),
        cross_ending_(cross_ending) {}

  asio::awaitable<void> start() {
    rest_rpc_header header;
    auto self = this->shared_from_this();
    while (true) {
      std::error_code ec;
      size_t size;
      std::tie(ec, size) = co_await asio::async_read(
          socket_, asio::buffer(&header, sizeof(rest_rpc_header)),
          asio::as_tuple(asio::use_awaitable));
      if (ec) {
        REST_LOG_INFO << "read head error: " << ec.message();
        break;
      }

      if (cross_ending_) {
        parse_recieved(header);
      }

      if (header.magic != REST_MAGIC_NUM) {
        REST_LOG_ERROR << "protocol error";
        break;
      }

      detail::resize(body_, header.body_len);

      if (header.body_len > 0) {
        std::tie(ec, size) = co_await asio::async_read(
            socket_, asio::buffer(body_), asio::as_tuple(asio::use_awaitable));
        if (ec) {
          REST_LOG_WARNING << "read body error: " << ec.message();
          break;
        }
      }

      // route
      rpc_context::context().set_connection(self);
      auto result = co_await router_.route(header.function_id, body_);
      bool delay = rpc_context::context().delay();
      if (delay) {
        rpc_context::context().set_delay(false);
        continue;
      }

      ec = co_await response(result);
      if (ec) {
        REST_LOG_WARNING << "write error: " << ec.message();
        break;
      }
    }
  }

  asio::awaitable<std::error_code> response(const rpc_result &result) {
    rest_rpc_header resp_header{};
    resp_header.magic = 39;
    resp_header.body_len = result.size() + 1;
    if (cross_ending_) {
      prepare_for_send(resp_header);
    }
    std::vector<asio::const_buffer> buffers;
    buffers.reserve(3);
    buffers.push_back(asio::buffer(&resp_header, sizeof(rest_rpc_header)));
    buffers.push_back(asio::buffer(&result.ec, 1));
    if (!result.empty())
      buffers.push_back(asio::buffer(result.data()));

    auto [ec, size] = co_await asio::async_write(
        socket_, buffers, asio::as_tuple(asio::use_awaitable));
    co_return ec;
  }

  uint64_t id() const { return conn_id_; }
  auto get_executor() { return socket_.get_executor(); }

private:
  tcp_socket socket_;
  uint64_t conn_id_;
  std::string body_;
  rpc_router &router_;
  bool cross_ending_;
};

// zero or one arguments
template <auto func, typename... Args>
asio::awaitable<std::error_code> rpc_context::response(Args &&...args) {
  using args_tuple = typename function_traits<decltype(func)>::return_type;
  static_assert(
      std::is_constructible_v<args_tuple, Args...>,
      "rpc function return type and response arguments are not match");
  if (has_response_) {
    co_return make_error_code(rpc_errc::has_response);
  }

  has_response_ = true;
  rpc_service::msgpack_codec codec;
  rpc_result result(codec.pack_args(std::forward<Args>(args)...));
  co_return co_await conn_->response(result);
}

template <auto func, typename... Args>
std::error_code rpc_context::sync_response(Args &&...args) {
  return sync_wait(conn_->get_executor(),
                   response<func>(std::forward<Args>(args)...));
}

auto rpc_context::get_executor() { return conn_->get_executor(); }
} // namespace rest_rpc