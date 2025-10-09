#pragma once
#include "io_context_pool.hpp"
#include "logger.hpp"
#include "rpc_connection.hpp"
#include "use_asio.hpp"
#include <string>
#include <thread>
namespace rest_rpc {
class rpc_server {
public:
  rpc_server(std::string address,
             size_t num_thread = std::thread::hardware_concurrency())
      : io_context_pool_(num_thread),
        acceptor_(io_context_pool_.get_io_context()),
        check_timer_(io_context_pool_.get_io_context()) {
    size_t pos = address.find(':');
    if (pos != std::string::npos) {
      host_ = address.substr(0, pos);
      port_ = address.substr(pos + 1);
    }
  }

  rpc_server(std::string host, std::string port,
             size_t num_thread = std::thread::hardware_concurrency())
      : io_context_pool_(num_thread),
        acceptor_(io_context_pool_.get_io_context()), host_(std::move(host)),
        port_(std::move(port)),
        check_timer_(io_context_pool_.get_io_context()) {}
  ~rpc_server() { stop(); }

  std::error_code start() { return start_impl(false); }

  std::error_code async_start() { return start_impl(true); }

  void set_conn_max_age(std::chrono::steady_clock::duration dur) {
    if (dur > std::chrono::steady_clock::duration::zero()) {
      need_check_ = true;
      timeout_duration_ = dur;
      asio::co_spawn(check_timer_.get_executor(), start_check_timer(),
                     asio::detached);
    }
  }

  void set_check_conn_interval(std::chrono::steady_clock::duration dur) {
    check_duration_ = dur;
  }

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

      stop_timer_ = true;
      REST_LOG_INFO << "server stoping";
      {
        std::scoped_lock lock(*conn_mtx_);
        for (auto &conn : conns_) {
          conn.second->close(false);
        }
        conns_.clear();
      }

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

  size_t connection_count() {
    std::scoped_lock lock(*conn_mtx_);
    return conns_.size();
  }

  auto get_connections() {
    std::scoped_lock lock(*conn_mtx_);
    return conns_;
  }

  template <typename T>
  asio::awaitable<void> publish(std::string_view topic, T &&t) {
    auto id = MD5::MD5Hash32(topic.data(), (uint32_t)topic.size());
    auto conns = get_connections();
    for (auto &[_, conn] : conns) {
      if (conn->topic_id() == id) {
        co_await conn->response(
            rpc_service::msgpack_codec::pack_args(std::forward<T>(t)), id);
      }
    }
  }

  template <typename T> void sync_publish(std::string_view topic, T &&t) {
    sync_wait(get_global_executor(), publish(topic, std::forward<T>(t)));
  }

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
      if (need_check_) {
        conn->set_check_timeout(true);
      }
      std::weak_ptr<std::mutex> weak(conn_mtx_);
      conn->set_quit_callback([this, weak](const uint64_t &id) {
        auto mtx = weak.lock();
        if (mtx) {
          std::scoped_lock lock(*mtx);
          if (!conns_.empty()) {
            std::erase_if(conns_, [id](const auto &pair) {
              if (pair.first == id) {
                pair.second->close(false);
                return true;
              }

              return false;
            });
          }
        }
      });

      {
        std::scoped_lock lock(*conn_mtx_);
        conns_.emplace(conn_id++, conn);
      }

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

  asio::awaitable<void> check_timeout() {
    auto cur_time = std::chrono::system_clock::now();

    {
      std::scoped_lock lock(*conn_mtx_);
      for (auto it = conns_.begin(); it != conns_.end();) {
        if (cur_time - co_await it->second->get_last_rwtime() >
            timeout_duration_) {
          it->second->close(false);
          conns_.erase(it++);
        } else {
          ++it;
        }
      }
    }
  }

  asio::awaitable<void> start_check_timer() {
    while (true) {
      check_timer_.expires_after(check_duration_);
      auto [ec] =
          co_await check_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
      if (ec || stop_timer_) {
        co_return;
      }
      co_await check_timeout();
    }
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

  std::shared_ptr<std::mutex> conn_mtx_ = std::make_shared<std::mutex>();
  std::chrono::steady_clock::duration check_duration_ =
      std::chrono::seconds(12);
  std::chrono::steady_clock::duration timeout_duration_{
      std::chrono::seconds(10)};
  asio::steady_timer check_timer_;
  std::atomic<bool> stop_timer_ = false;
  bool need_check_ = false;

  rpc_router router_;
  bool tcp_no_delay_ = true;
  bool cross_ending_ = false;
};
} // namespace rest_rpc