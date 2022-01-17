#ifndef REST_RPC_CONNECTION_H_
#define REST_RPC_CONNECTION_H_

#include "const_vars.h"
#include "cplusplus_14.h"
#include "nonstd_any.hpp"
#include "router.h"
#include "use_asio.hpp"
#include <array>
#include <deque>
#include <iostream>
#include <memory>

using asio::ip::tcp;

namespace rest_rpc {
namespace rpc_service {
struct ssl_configure {
  std::string cert_file;
  std::string key_file;
};

class connection : public std::enable_shared_from_this<connection>,
                   private asio::noncopyable {
public:
  connection(asio::io_service &io_service, std::size_t timeout_seconds,
             router &router)
      : socket_(io_service), body_(INIT_BUF_SIZE), timer_(io_service),
        timeout_seconds_(timeout_seconds), has_closed_(false), router_(router) {
  }

  ~connection() { close(); }

  void start() {
    if (is_ssl() && !has_shake_) {
      async_handshake();
    } else {
      read_head();
    }
  }

  tcp::socket &socket() { return socket_; }

  bool has_closed() const { return has_closed_; }
  uint64_t request_id() const { return req_id_; }

  void response(uint64_t req_id, std::string data,
                request_type req_type = request_type::req_res) {
    auto len = data.size();
    assert(len < MAX_BUF_LEN);

    std::unique_lock<std::mutex> lock(write_mtx_);
    write_queue_.emplace_back(message_type{
        req_id, req_type, std::make_shared<std::string>(std::move(data))});
    if (write_queue_.size() > 1) {
      return;
    }

    write();
  }

  template <typename T> void pack_and_response(uint64_t req_id, T data) {
    auto result =
        msgpack_codec::pack_args_str(result_code::OK, std::move(data));
    response(req_id, std::move(result));
  }

  void set_conn_id(int64_t id) { conn_id_ = id; }

  int64_t conn_id() const { return conn_id_; }

  template <typename T> void set_user_data(const T &data) { user_data_ = data; }

  template <typename T> T get_user_data() {
    return nonstd::any_cast<T>(user_data_);
  }

  const std::vector<char> &body() const { return body_; }

  std::string remote_address() const {
    if (has_closed_) {
      return "";
    }

    asio::error_code ec;
    auto endpoint = socket_.remote_endpoint(ec);
    if (ec) {
      return "";
    }
    return endpoint.address().to_string();
  }

  void publish(const std::string &key, const std::string &data) {
    auto result = msgpack_codec::pack_args_str(result_code::OK, key, data);
    response(0, std::move(result), request_type::sub_pub);
  }

  void set_callback(
      std::function<void(std::string, std::string, std::weak_ptr<connection>)>
          callback) {
    callback_ = std::move(callback);
  }

  void on_network_error(std::function<void(std::shared_ptr<connection>,
                                           std::string)> &on_net_err) {
    on_net_err_ = &on_net_err;
  }

  void init_ssl_context(const ssl_configure &ssl_conf) {
#ifdef CINATRA_ENABLE_SSL
    unsigned long ssl_options = asio::ssl::context::default_workarounds |
                                asio::ssl::context::no_sslv2 |
                                asio::ssl::context::single_dh_use;
    try {
      asio::ssl::context ssl_context(asio::ssl::context::sslv23);
      ssl_context.set_options(ssl_options);
      ssl_context.set_password_callback(
          [](std::size_t size,
             asio::ssl::context_base::password_purpose purpose) {
            return "123456";
          });

      asio::error_code ec;
      if (rpcfs::exists(ssl_conf.cert_file, ec)) {
        ssl_context.use_certificate_chain_file(ssl_conf.cert_file);
      }

      if (rpcfs::exists(ssl_conf.key_file, ec))
        ssl_context.use_private_key_file(ssl_conf.key_file,
                                         asio::ssl::context::pem);

      // ssl_context_callback(ssl_context);
      ssl_stream_ =
          std::make_unique<asio::ssl::stream<asio::ip::tcp::socket &>>(
              socket_, ssl_context);
    } catch (const std::exception &e) {
      print(e);
    }
#endif
  }

private:
  void read_head() {
    reset_timer();
    auto self(this->shared_from_this());
    async_read_head([this, self](asio::error_code ec, std::size_t length) {
      if (!socket_.is_open()) {
        if (on_net_err_) {
          (*on_net_err_)(self, "socket already closed");
        }
        return;
      }

      if (!ec) {
        // const uint32_t body_len = *((int*)(head_));
        // req_id_ = *((std::uint64_t*)(head_ + sizeof(int32_t)));
        rpc_header *header = (rpc_header *)(head_);
        req_id_ = header->req_id;
        const uint32_t body_len = header->body_len;
        req_type_ = header->req_type;
        if (body_len > 0 && body_len < MAX_BUF_LEN) {
          if (body_.size() < body_len) {
            body_.resize(body_len);
          }
          read_body(body_len);
          return;
        }

        if (body_len == 0) { // nobody, just head, maybe as heartbeat.
          cancel_timer();
          read_head();
        } else {
          print("invalid body len");
          if (on_net_err_) {
            (*on_net_err_)(self, "invalid body len");
          }
          close();
        }
      } else {
        print(ec);
        if (on_net_err_) {
          (*on_net_err_)(self, ec.message());
        }
        close();
      }
    });
  }

  void read_body(std::size_t size) {
    auto self(this->shared_from_this());
    async_read(size, [this, self](asio::error_code ec, std::size_t length) {
      cancel_timer();

      if (!socket_.is_open()) {
        if (on_net_err_) {
          (*on_net_err_)(self, "socket already closed");
        }
        return;
      }

      if (!ec) {
        read_head();
        if (req_type_ == request_type::req_res) {
          router_.route<connection>(body_.data(), length,
                                    this->shared_from_this());
        } else if (req_type_ == request_type::sub_pub) {
          try {
            msgpack_codec codec;
            auto p = codec.unpack<std::tuple<std::string, std::string>>(
                body_.data(), length);
            callback_(std::move(std::get<0>(p)), std::move(std::get<1>(p)),
                      this->shared_from_this());
          } catch (const std::exception &ex) {
            print(ex);
            if (on_net_err_) {
              (*on_net_err_)(self, ex.what());
            }
          }
        }
      } else {
        if (on_net_err_) {
          (*on_net_err_)(self, ec.message());
        }
      }
    });
  }

  void write() {
    auto &msg = write_queue_.front();
    write_size_ = (uint32_t)msg.content->size();
    std::array<asio::const_buffer, 4> write_buffers;
    write_buffers[0] = asio::buffer(&write_size_, sizeof(uint32_t));
    write_buffers[1] = asio::buffer(&msg.req_id, sizeof(uint64_t));
    write_buffers[2] = asio::buffer(&msg.req_type, sizeof(request_type));
    write_buffers[3] = asio::buffer(msg.content->data(), write_size_);

    auto self = this->shared_from_this();
    async_write(write_buffers,
                [this, self](asio::error_code ec, std::size_t length) {
                  on_write(ec, length);
                });
  }

  void on_write(asio::error_code ec, std::size_t length) {
    if (ec) {
      print(ec);
      if (on_net_err_) {
        (*on_net_err_)(shared_from_this(), ec.message());
      }
      close(false);
      return;
    }

    if (has_closed()) {
      return;
    }

    std::unique_lock<std::mutex> lock(write_mtx_);
    write_queue_.pop_front();

    if (!write_queue_.empty()) {
      write();
    }
  }

  void async_handshake() {
#ifdef CINATRA_ENABLE_SSL
    auto self = this->shared_from_this();
    ssl_stream_->async_handshake(asio::ssl::stream_base::server,
                                 [this, self](const asio::error_code &error) {
                                   if (error) {
                                     print(error);
                                     if (on_net_err_) {
                                       (*on_net_err_)(self, error.message());
                                     }
                                     close();
                                     return;
                                   }

                                   has_shake_ = true;
                                   read_head();
                                 });
#endif
  }

  bool is_ssl() const {
#ifdef CINATRA_ENABLE_SSL
    return ssl_stream_ != nullptr;
#else
    return false;
#endif
  }

  template <typename Handler> void async_read_head(Handler handler) {
    if (is_ssl()) {
#ifdef CINATRA_ENABLE_SSL
      asio::async_read(*ssl_stream_, asio::buffer(head_, HEAD_LEN),
                       std::move(handler));
#endif
    } else {
      asio::async_read(socket_, asio::buffer(head_, HEAD_LEN),
                       std::move(handler));
    }
  }

  template <typename Handler>
  void async_read(size_t size_to_read, Handler handler) {
    if (is_ssl()) {
#ifdef CINATRA_ENABLE_SSL
      asio::async_read(*ssl_stream_, asio::buffer(body_.data(), size_to_read),
                       std::move(handler));
#endif
    } else {
      asio::async_read(socket_, asio::buffer(body_.data(), size_to_read),
                       std::move(handler));
    }
  }

  template <typename BufferType, typename Handler>
  void async_write(const BufferType &buffers, Handler handler) {
    if (is_ssl()) {
#ifdef CINATRA_ENABLE_SSL
      asio::async_write(*ssl_stream_, buffers, std::move(handler));
#endif
    } else {
      asio::async_write(socket_, buffers, std::move(handler));
    }
  }

  void reset_timer() {
    if (timeout_seconds_ == 0) {
      return;
    }

    auto self(this->shared_from_this());
    timer_.expires_from_now(std::chrono::seconds(timeout_seconds_));
    timer_.async_wait([this, self](const asio::error_code &ec) {
      if (has_closed()) {
        return;
      }

      if (ec) {
        return;
      }

      // LOG(INFO) << "rpc connection timeout";
      close(false);
    });
  }

  void cancel_timer() {
    if (timeout_seconds_ == 0) {
      return;
    }

    timer_.cancel();
  }

  void close(bool close_ssl = true) {
#ifdef CINATRA_ENABLE_SSL
    if (close_ssl && ssl_stream_) {
      asio::error_code ec;
      ssl_stream_->shutdown(ec);
      ssl_stream_ = nullptr;
    }
#endif
    if (has_closed_) {
      return;
    }

    asio::error_code ignored_ec;
    socket_.shutdown(tcp::socket::shutdown_both, ignored_ec);
    socket_.close(ignored_ec);
    has_closed_ = true;
    has_shake_ = false;
  }

  template <typename... Args> void print(Args... args) {
#ifdef _DEBUG
    std::initializer_list<int>{(std::cout << args << ' ', 0)...};
    std::cout << "\n";
#endif
  }

  void print(const asio::error_code &ec) { print(ec.value(), ec.message()); }

  void print(const std::exception &ex) { print(ex.what()); }

  tcp::socket socket_;
#ifdef CINATRA_ENABLE_SSL
  std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket &>> ssl_stream_ =
      nullptr;
#endif
  bool has_shake_ = false;
  char head_[HEAD_LEN];
  std::vector<char> body_;
  std::uint64_t req_id_;
  request_type req_type_;

  uint32_t write_size_ = 0;
  std::mutex write_mtx_;

  asio::steady_timer timer_;
  std::size_t timeout_seconds_;
  int64_t conn_id_ = 0;
  bool has_closed_;

  std::deque<message_type> write_queue_;
  std::function<void(std::string, std::string, std::weak_ptr<connection>)>
      callback_;
  std::function<void(std::shared_ptr<connection>, std::string)> *on_net_err_ =
      nullptr;
  router &router_;
  nonstd::any user_data_;
};
} // namespace rpc_service
} // namespace rest_rpc

#endif // REST_RPC_CONNECTION_H_
