#pragma once
#include "logger.hpp"
#include "rest_rpc_protocol.hpp"
#include "rpc_router.hpp"
#include "string_resize.hpp"
#include "use_asio.hpp"
#include <cstring>
#include <deque>

namespace rest_rpc {
class rpc_connection;

class tls_data {
public:
  void set_connection(std::shared_ptr<rpc_connection> conn) { conn_ = conn; }
  void set_seq_num(uint64_t seq_num) { seq_num_ = seq_num; }
  uint64_t seq_num() const { return seq_num_; }

  bool delay() { return delay_; }
  void set_delay(bool r) { delay_ = r; }

  auto get_executor();
  std::shared_ptr<rpc_connection> get_conn() { return conn_; }

private:
  std::shared_ptr<rpc_connection> conn_ = nullptr;
  uint64_t seq_num_ = 0;
  bool delay_ = false;
};

inline auto &get_context() {
  static thread_local tls_data instance;
  return instance;
}

class rpc_context {
public:
  rpc_context();

  auto get_executor();

  template <auto func, typename... Args>
  asio::awaitable<std::error_code> response_s(Args &&...args);

  template <typename... Args>
  asio::awaitable<std::error_code> response(Args &&...args);

private:
  asio::any_io_executor executor_;
  std::shared_ptr<rpc_connection> conn_ = nullptr;
  uint64_t seq_num_ = 0;
  std::shared_ptr<bool> has_response_ = std::make_shared<bool>(false);
};

class rpc_connection : public std::enable_shared_from_this<rpc_connection> {
  struct write_operation {
    write_operation(asio::any_io_executor executor, std::string data)
        : notify_(executor, (std::chrono::steady_clock::time_point::max)()),
          data_(std::move(data)) {}

    void complete(std::error_code ec) {
      if (completed_) {
        return;
      }
      ec_ = ec;
      completed_ = true;
      notify_.cancel();
    }

    asio::steady_timer notify_;
    std::string data_;
    std::error_code ec_;
    bool completed_ = false;
  };

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
      set_last_time();
      std::tie(ec, size) = co_await asio::async_read(
          socket_, asio::buffer(&header, sizeof(rest_rpc_header)),
          asio::as_tuple(asio::use_awaitable));
      if (ec) {
        if (ec != asio::error::eof) {
          REST_LOG_INFO << "read head error: " << ec.message();
        } else {
          REST_LOG_INFO << "read head error: " << ec.message();
        }
        close();
        break;
      }

      if (cross_ending_) {
        parse_recieved(header);
      }
      if (header.magic != REST_MAGIC_NUM) {
        REST_LOG_ERROR << "protocol error";
        close();
        break;
      }
      if (header.msg_type > 1 || header.attach_length != 0 ||
          header.body_len > body_.max_size() ||
          (header.msg_type == 1 && header.body_len != 0)) {
        REST_LOG_ERROR << "protocol error";
        close();
        break;
      }

      if (header.msg_type == 1) { // pub sub
        topic_id_ = header.function_id;
        continue;
      }

      try {
        detail::resize(body_, static_cast<size_t>(header.body_len));
      } catch (...) {
        REST_LOG_ERROR << "resize body failed";
        close();
        break;
      }
      if (header.body_len > 0) {
        set_last_time();
        std::tie(ec, size) = co_await asio::async_read(
            socket_, asio::buffer(body_), asio::as_tuple(asio::use_awaitable));
        if (ec) {
          REST_LOG_WARNING << "read body error: " << ec.message();
          close();
          break;
        }
      }

      // route
      get_context().set_connection(self);
      get_context().set_seq_num(header.seq_num);
      get_context().set_delay(false);
      auto result = co_await router_.route(header.function_id, body_);
      bool delay = get_context().delay();
      if (delay) {
        get_context().set_delay(false);
        continue;
      }

      ec = co_await response_with_seq(result, 0, header.seq_num);
      if (ec) {
        REST_LOG_WARNING << "write error: " << ec.message();
        break;
      }
    }
  }

  asio::awaitable<std::error_code> response(const rpc_result &result,
                                            uint32_t func_id = 0) {
    return response_with_seq(result, func_id, 0);
  }

  uint64_t id() const { return conn_id_; }
  auto get_executor() { return socket_.get_executor(); }
  uint32_t topic_id() const { return topic_id_; }

  void
  set_quit_callback(std::function<void(const uint64_t &conn_id)> callback) {
    quit_cb_ = std::move(callback);
  }

  void close(bool need_cb = true) {
    if (has_closed_) {
      return;
    }

    auto self = shared_from_this();
    asio::dispatch(socket_.get_executor(), [this, need_cb, self] {
      std::error_code ec;
      socket_.shutdown(asio::socket_base::shutdown_both, ec);
      socket_.close(ec);
      REST_LOG_INFO << "close connection, id " << conn_id_;
      if (need_cb && quit_cb_) {
        quit_cb_(conn_id_);
      }
      has_closed_ = true;
    });
  }

  void set_last_time() {
    if (checkout_timeout_) {
      last_rwtime_ = std::chrono::system_clock::now();
    }
  }

  asio::awaitable<std::chrono::system_clock::time_point> get_last_rwtime() {
    co_await asio::this_coro::executor;
    co_return last_rwtime_;
  }

  void set_check_timeout(bool r) { checkout_timeout_ = r; }

private:
  friend class rpc_context;

  asio::awaitable<std::error_code> response_with_seq(const rpc_result &result,
                                                     uint32_t func_id,
                                                     uint64_t seq_num) {
    rest_rpc_header resp_header{};
    resp_header.magic = 39;
    resp_header.seq_num = seq_num;
    if (func_id != 0) {
      resp_header.msg_type = 1;
      resp_header.function_id = func_id;
      resp_header.seq_num = 0;
    }
    resp_header.body_len = result.size() + 1;
    if (cross_ending_) {
      prepare_for_send(resp_header);
    }

    auto body = result.data();
    std::string frame(sizeof(rest_rpc_header) + 1 + body.size(), '\0');
    std::memcpy(frame.data(), &resp_header, sizeof(rest_rpc_header));
    std::memcpy(frame.data() + sizeof(rest_rpc_header), &result.ec, 1);
    if (!body.empty()) {
      std::memcpy(frame.data() + sizeof(rest_rpc_header) + 1, body.data(),
                  body.size());
    }

    co_return co_await asio::co_spawn(
        get_executor(), enqueue_response(shared_from_this(), std::move(frame)),
        asio::use_awaitable);
  }

  static asio::awaitable<std::error_code>
  enqueue_response(std::shared_ptr<rpc_connection> self, std::string frame) {
    if (self->has_closed_) {
      co_return make_error_code(rpc_errc::socket_closed);
    }

    auto operation = std::make_shared<write_operation>(self->get_executor(),
                                                       std::move(frame));
    self->write_queue_.push_back(operation);
    if (!self->write_in_progress_) {
      self->write_in_progress_ = true;
      asio::co_spawn(self->get_executor(), write_loop(self), asio::detached);
    }

    if (!operation->completed_) {
      auto [wait_ec] = co_await operation->notify_.async_wait(
          asio::as_tuple(asio::use_awaitable));
      if (!operation->completed_) {
        co_return asio::error::operation_aborted;
      }
    }
    if (operation->ec_) {
      REST_LOG_WARNING << "write error: " << operation->ec_.message();
      self->close();
    }
    co_return operation->ec_;
  }
  static asio::awaitable<void>
  write_loop(std::shared_ptr<rpc_connection> self) {
    while (!self->write_queue_.empty()) {
      auto operation = self->write_queue_.front();
      auto [ec, size] = co_await asio::async_write(
          self->socket_, asio::buffer(operation->data_),
          asio::as_tuple(asio::use_awaitable));
      self->write_queue_.pop_front();
      operation->complete(ec);
      if (ec) {
        while (!self->write_queue_.empty()) {
          auto queued = self->write_queue_.front();
          self->write_queue_.pop_front();
          queued->complete(ec);
        }
        self->write_in_progress_ = false;
        co_return;
      }
      self->set_last_time();
    }
    self->write_in_progress_ = false;
  }

  tcp_socket socket_;
  uint64_t conn_id_;
  std::string body_;
  std::function<void(const uint64_t &conn_id)> quit_cb_ = nullptr;
  std::atomic<bool> has_closed_{false};
  std::chrono::system_clock::time_point last_rwtime_ =
      std::chrono::system_clock::now();
  bool checkout_timeout_ = false;
  rpc_router &router_;
  bool cross_ending_;
  std::atomic<uint32_t> topic_id_;
  bool write_in_progress_ = false;
  std::deque<std::shared_ptr<write_operation>> write_queue_;
};

auto tls_data::get_executor() {
  if (!conn_) {
    return asio::any_io_executor();
  }
  return conn_->get_executor();
}

auto rpc_context::get_executor() { return executor_; }

rpc_context::rpc_context() {
  executor_ = get_context().get_executor();
  conn_ = get_context().get_conn();
  seq_num_ = get_context().seq_num();
  get_context().set_delay(true);
}

template <auto func, typename... Args>
asio::awaitable<std::error_code> rpc_context::response_s(Args &&...args) {
  using args_tuple =
      typename util::function_traits<decltype(func)>::return_type;
  static_assert(
      std::is_constructible_v<args_tuple, Args...>,
      "rpc function return type and response arguments are not match");
  return response(std::forward<Args>(args)...);
}

template <typename... Args>
asio::awaitable<std::error_code> rpc_context::response(Args &&...args) {
  if (*has_response_) {
    co_return make_error_code(rpc_errc::has_response);
  }
  if (!conn_) {
    REST_LOG_ERROR << "rpc context init failed";
    co_return make_error_code(rpc_errc::rpc_context_init_failed);
  }

  rpc_result result(rpc_codec::pack_args(std::forward<Args>(args)...));
  *has_response_ = true;
  co_return co_await conn_->response_with_seq(result, 0, seq_num_);
}

} // namespace rest_rpc
