#pragma once
#include "io_context_pool.hpp"
#include "logger.hpp"
#include "rpc_connection.hpp"
#include "use_asio.hpp"
#include <string>
#include <thread>
namespace rest_rpc {
class rest_rpc_server {
public:
  rest_rpc_server(std::string address,
                  size_t num_thread = std::thread::hardware_concurrency())
      : io_context_pool_(num_thread),
        acceptor_(io_context_pool_.get_io_context()) {
    size_t pos = address.find(':');
    if (pos != std::string::npos) {
      host_ = address.substr(0, pos);
      port_ = address.substr(pos + 1);
    }
  }

  rest_rpc_server(std::string host, std::string port,
                  size_t num_thread = std::thread::hardware_concurrency())
      : io_context_pool_(num_thread),
        acceptor_(io_context_pool_.get_io_context()), host_(std::move(host)),
        port_(std::move(port)) {}
  ~rest_rpc_server() { stop(); }

  std::error_code start() { return start_impl(false); }

  std::error_code async_start() { return start_impl(true); }

  void stop() {
    if (has_stop_.load(std::memory_order_acquire)) {
      return;
    }

    std::call_once(stop_flag_, [this] {
      has_stop_.store(true, std::memory_order_release);
      asio::dispatch(acceptor_.get_executor(), [this]() {
        asio::error_code ec;
        (void)acceptor_.cancel(ec);
        (void)acceptor_.close(ec);
      });

      io_context_pool_.stop();
    });

    if (thd_.joinable()) {
      thd_.join();
    }
  }

  bool has_stopped() const { return has_stop_.load(std::memory_order_acquire); }

  template <typename Function, typename Self = void>
  void register_handler(std::string_view name, const Function &f,
                        Self *self = nullptr) {
    router_.register_handler(name, f, self);
  }

  template <auto func, typename Self = void>
  void register_handler(Self *self = nullptr) {
    router_.register_handler<func>(self);
  }

  void remove_handler(std::string_view name) { router_.remove_handler(name); }

  template <auto func> void remove_handler() { router_.remove_handler<func>(); }

  void enable_tcp_no_delay(bool r) { tcp_no_delay_ = r; }

  void enable_cross_ending(bool r) { cross_ending_ = r; }

private:
  std::error_code listen() {
    using asio::ip::tcp;
    asio::error_code ec;
    asio::ip::tcp::resolver resolver(acceptor_.get_executor());
    auto endpoints = resolver.resolve(host_, port_, ec);
    if (ec) {
      return ec;
    }

    auto it = endpoints.begin();

    if (it == endpoints.end()) {
      return std::make_error_code(std::errc::bad_address);
    }

    auto endpoint = it->endpoint();
    acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
      return ec;
    }

#ifdef __GNUC__
    acceptor_.set_option(tcp::acceptor::reuse_address(true), ec);
#endif
    acceptor_.bind(endpoint, ec);
    if (ec) {
      std::error_code ignore;
      acceptor_.cancel(ignore);
      acceptor_.close(ignore);
      return ec;
    }
#ifdef _MSC_VER
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
#endif
    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
      std::error_code ignore;
      acceptor_.cancel(ignore);
      acceptor_.close(ignore);
      return ec;
    }
    return ec;
  }

  asio::awaitable<void> accept() {
    uint64_t conn_id = 0;
    while (true) {
      tcp_socket socket(io_context_pool_.get_io_context());
      auto [ec] = co_await acceptor_.async_accept(
          socket, asio::as_tuple(asio::use_awaitable));
      if (ec == asio::error::operation_aborted ||
          ec == asio::error::bad_descriptor) {
        REST_LOG_WARNING << "acceptor error: " << ec.message();
        co_return;
      }

      if (tcp_no_delay_) {
        socket.set_option(asio::ip::tcp::no_delay(true));
      }

      REST_LOG_INFO << "new connction comming...";
      auto conn = std::make_shared<rpc_connection>(std::move(socket), conn_id,
                                                   router_, cross_ending_);
      conns_.emplace(conn_id++, conn);
      co_spawn(socket.get_executor(), conn->start(), asio::detached);
    }
  }

  std::error_code start_impl(bool async) {
    if (has_stop_.load(std::memory_order_acquire)) {
      return std::make_error_code(std::errc::operation_canceled);
    }

    static std::error_code ec{};
    std::call_once(start_flag_, [this, async] {
      ec = listen();
      if (ec) {
        return;
      }

      thd_ = std::thread([this] { io_context_pool_.run(); });

      if (async) {
        asio::co_spawn(acceptor_.get_executor(), accept(), asio::detached);
      } else {
        auto future = asio::co_spawn(acceptor_.get_executor(), accept(),
                                     asio::use_future);
        future.wait();
      }
    });

    return ec;
  }

  io_context_pool io_context_pool_;
  std::thread thd_;
  asio::ip::tcp::acceptor acceptor_;
  std::string host_;
  std::string port_;
  std::once_flag start_flag_;
  std::once_flag stop_flag_;
  std::atomic<bool> has_stop_ = false;
  std::unordered_map<uint64_t, std::shared_ptr<rpc_connection>> conns_;
  rpc_router router_;
  bool tcp_no_delay_ = true;
  bool cross_ending_ = false;
};
} // namespace rest_rpc