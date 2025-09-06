#ifndef REST_RPC_RPC_SERVER_H_
#define REST_RPC_RPC_SERVER_H_

#include "connection.h"
#include "io_service_pool.h"
#include "router.h"
#include <condition_variable>
#include <mutex>
#include <thread>
using asio::ip::tcp;

namespace rest_rpc {
namespace rpc_service {
using rpc_conn = std::weak_ptr<connection>;
class rpc_server : private asio::noncopyable {
public:
  rpc_server(unsigned short port, size_t size, size_t timeout_seconds = 15,
             size_t check_seconds = 10)
      : io_service_pool_(size), acceptor_(io_service_pool_.get_io_service(),
                                          tcp::endpoint(tcp::v4(), port)),
        timeout_seconds_(timeout_seconds), check_seconds_(check_seconds),
        signals_(io_service_pool_.get_io_service()) {
    do_accept();
    check_thread_ = std::make_shared<std::thread>([this] { clean(); });
    pub_sub_thread_ =
        std::make_shared<std::thread>([this] { clean_sub_pub(); });
    signals_.add(SIGINT);
    signals_.add(SIGTERM);
#if defined(SIGQUIT)
    signals_.add(SIGQUIT);
#endif // defined(SIGQUIT)
    do_await_stop();
  }

  rpc_server(unsigned short port, size_t size, ssl_configure ssl_conf,
             size_t timeout_seconds = 15, size_t check_seconds = 10)
      : rpc_server(port, size, timeout_seconds, check_seconds) {
#ifdef CINATRA_ENABLE_SSL
    ssl_conf_ = std::move(ssl_conf);
#else
    assert(false); // please add definition CINATRA_ENABLE_SSL, not allowed
                   // coming in this branch
#endif
  }

  ~rpc_server() { stop(); }

  void async_run() {
    thd_ = std::make_shared<std::thread>([this] { io_service_pool_.run(); });
  }

  void run() { io_service_pool_.run(); }

  template <bool is_pub = false, typename Function>
  void register_handler(std::string const &name, const Function &f) {
    router_.register_handler<is_pub>(name, f);
  }

  template <bool is_pub = false, typename Function, typename Self>
  void register_handler(std::string const &name, const Function &f,
                        Self *self) {
    router_.register_handler<is_pub>(name, f, self);
  }

  void
  set_error_callback(std::function<void(asio::error_code, string_view)> f) {
    err_cb_ = std::move(f);
  }

  void set_conn_timeout_callback(std::function<void(int64_t)> callback) {
    conn_timeout_callback_ = std::move(callback);
  }

  void set_network_err_callback(
      std::function<void(std::shared_ptr<connection>, std::string /*reason*/)>
          on_net_err) {
    on_net_err_callback_ = std::move(on_net_err);
  }

  template <typename T> void publish(const std::string &key, T data) {
    publish(key, "", std::move(data));
  }

  template <typename T>
  void publish_by_token(const std::string &key, std::string token, T data) {
    publish(key, std::move(token), std::move(data));
  }

  std::set<std::string> get_token_list() {
    std::unique_lock<std::mutex> lock(sub_mtx_);
    return token_list_;
  }

private:
  void do_accept() {
    conn_.reset(new connection(io_service_pool_.get_io_service(),
                               timeout_seconds_, router_));
    conn_->set_callback([this](std::string key, std::string token,
                               std::weak_ptr<connection> conn) {
      std::unique_lock<std::mutex> lock(sub_mtx_);
      sub_map_.emplace(std::move(key) + token, conn);
      if (!token.empty()) {
        token_list_.emplace(std::move(token));
      }
    });
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.async_accept(conn_->socket(), [this](asio::error_code ec) {
      if (!acceptor_.is_open()) {
        return;
      }

      if (ec) {
        // LOG(INFO) << "acceptor error: " <<
        // ec.message();
      } else {
#ifdef CINATRA_ENABLE_SSL
        if (!ssl_conf_.cert_file.empty()) {
          conn_->init_ssl_context(ssl_conf_);
        }
#endif
        if (on_net_err_callback_) {
          conn_->on_network_error(on_net_err_callback_);
        }
        conn_->start();
        std::unique_lock<std::mutex> lock(mtx_);
        conn_->set_conn_id(conn_id_);
        connections_.emplace(conn_id_++, conn_);
      }

      do_accept();
    });
  }

  void clean() {
    while (!stop_check_) {
      std::unique_lock<std::mutex> lock(mtx_);
      cv_.wait_for(lock, std::chrono::seconds(check_seconds_));

      for (auto it = connections_.cbegin(); it != connections_.cend();) {
        if (it->second->has_closed()) {
          if (conn_timeout_callback_) {
            conn_timeout_callback_(it->second->conn_id());
          }
          it = connections_.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  void clean_sub_pub() {
    while (!stop_check_pub_sub_) {
      std::unique_lock<std::mutex> lock(sub_mtx_);
      sub_cv_.wait_for(lock, std::chrono::seconds(10));

      for (auto it = sub_map_.cbegin(); it != sub_map_.cend();) {
        auto conn = it->second.lock();
        if (conn == nullptr || conn->has_closed()) {
          // remove token
          for (auto t = token_list_.begin(); t != token_list_.end();) {
            if (it->first.find(*t) != std::string::npos) {
              t = token_list_.erase(t);
            } else {
              ++t;
            }
          }

          it = sub_map_.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  void error_callback(const asio::error_code &ec, string_view msg) {
    if (err_cb_) {
      err_cb_(ec, msg);
    }
  }
  template <typename T>
  void publish(std::string key, std::string token, T data) {
    {
      std::unique_lock<std::mutex> lock(sub_mtx_);
      if (sub_map_.empty())
        return;
    }

    std::shared_ptr<std::string> shared_data =
        get_shared_data<T>(std::move(data));
    std::unique_lock<std::mutex> lock(sub_mtx_);
    auto range = sub_map_.equal_range(key + token);
    if (range.first != range.second) {
      for (auto it = range.first; it != range.second; ++it) {
        auto conn = it->second.lock();
        if (conn == nullptr || conn->has_closed()) {
          continue;
        }

        conn->publish(key + token, *shared_data);
      }
    } else {
      error_callback(
          asio::error::make_error_code(asio::error::invalid_argument),
          "The subscriber of the key: " + key + " does not exist.");
    }
  }

  template <typename T>
  typename std::enable_if<std::is_assignable<std::string, T>::value,
                          std::shared_ptr<std::string>>::type
  get_shared_data(std::string data) {
    return std::make_shared<std::string>(std::move(data));
  }

  template <typename T>
  typename std::enable_if<!std::is_assignable<std::string, T>::value,
                          std::shared_ptr<std::string>>::type
  get_shared_data(T data) {
    msgpack_codec codec;
    auto buf = codec.pack(std::move(data));
    return std::make_shared<std::string>(buf.data(), buf.size());
  }
  void do_await_stop() {
    signals_.async_wait(
        [this](std::error_code /*ec*/, int /*signo*/) { 
              std::cerr << "server terminate!\n";
          stop(); });
  }
  void stop() {
    if (has_stoped_) {
      return;
    }

    {
      std::unique_lock<std::mutex> lock(mtx_);
      stop_check_ = true;
      cv_.notify_all();
    }
    check_thread_->join();

    {
      std::unique_lock<std::mutex> lock(sub_mtx_);
      stop_check_pub_sub_ = true;
      sub_cv_.notify_all();
    }
    pub_sub_thread_->join();

    io_service_pool_.stop();
    if (thd_) {
      thd_->join();
    }
    has_stoped_ = true;
  }

  io_service_pool io_service_pool_;
  tcp::acceptor acceptor_;
  std::shared_ptr<connection> conn_;
  std::shared_ptr<std::thread> thd_;
  std::size_t timeout_seconds_;

  std::unordered_map<int64_t, std::shared_ptr<connection>> connections_;
  int64_t conn_id_ = 0;
  std::mutex mtx_;
  std::shared_ptr<std::thread> check_thread_;
  size_t check_seconds_;
  bool stop_check_ = false;
  std::condition_variable cv_;

  asio::signal_set signals_;

  std::function<void(asio::error_code, string_view)> err_cb_;
  std::function<void(int64_t)> conn_timeout_callback_;
  std::function<void(std::shared_ptr<connection>, std::string)>
      on_net_err_callback_ = nullptr;
  std::unordered_multimap<std::string, std::weak_ptr<connection>> sub_map_;
  std::set<std::string> token_list_;
  std::mutex sub_mtx_;
  std::condition_variable sub_cv_;

  std::shared_ptr<std::thread> pub_sub_thread_;
  bool stop_check_pub_sub_ = false;

  ssl_configure ssl_conf_;
  router router_;
  std::atomic_bool has_stoped_ = {false};
};
} // namespace rpc_service
} // namespace rest_rpc

#endif // REST_RPC_RPC_SERVER_H_
