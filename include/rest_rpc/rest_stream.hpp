#pragma once
#include "error_code.h"
#include "logger.hpp"
#include "string_resize.hpp"
#include "use_asio.hpp"

namespace rest_rpc {
template <typename T> class rest_stream {
public:
  rest_stream(T t, size_t init_size)
      : io_object_(t), socket_(io_object_->get_socket()) {
    detail::resize(data_, init_size);
  }

  asio::awaitable<std::pair<rpc_errc, std::string_view>> recv() {
    rpc_errc code{};
    auto [ec, size] = co_await socket_.async_read_some(
        asio::buffer(data_), asio::as_tuple(asio::use_awaitable));
    if (ec) {
      REST_LOG_WARNING << "recv error: " << ec.message();
      code = rpc_errc::read_error;
    }

    co_return std::make_pair(code, std::string_view(data_, size));
  }

  asio::awaitable<rpc_errc> send(std::string_view data) {
    rpc_errc code{};
    auto [ec, size] = co_await asio::async_write(
        socket_, asio::buffer(data), asio::as_tuple(asio::use_awaitable));
    if (ec) {
      REST_LOG_WARNING << "write error: " << ec.message();
      code = rpc_errc::write_error;
    }
    co_return code;
  }

private:
  T io_object_;
  asio::ip::tcp::socket &socket_;
  std::string data_;
};

template <typename T> rest_stream(T) -> rest_stream<T>;
} // namespace rest_rpc