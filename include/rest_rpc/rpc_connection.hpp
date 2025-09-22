#pragma once
#include "logger.hpp"
#include "rest_rpc_protocol.hpp"
#include "rpc_router.hpp"
#include "string_resize.hpp"
#include "use_asio.hpp"

namespace rest_rpc {
class rpc_connection {
public:
  rpc_connection(tcp_socket socket, uint64_t conn_id, rpc_router &router,
                 bool &cross_ending)
      : socket_(std::move(socket)), conn_id_(conn_id), router_(router),
        cross_ending_(cross_ending) {}

  asio::awaitable<void> start() {
    rest_rpc_header header;

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
      auto result = router_.route(header.function_id, body_);
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

      std::tie(ec, size) = co_await asio::async_write(
          socket_, buffers, asio::as_tuple(asio::use_awaitable));
      if (ec) {
        REST_LOG_WARNING << "write error: " << ec.message();
        break;
      }
    }

    co_return;
  }

  uint64_t id() const { return conn_id_; }

private:
  tcp_socket socket_;
  uint64_t conn_id_;
  std::string body_;
  rpc_router &router_;
  bool cross_ending_;
};
} // namespace rest_rpc