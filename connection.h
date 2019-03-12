#ifndef REST_RPC_CONNECTION_H_
#define REST_RPC_CONNECTION_H_

#include <iostream>
#include <memory>
#include <array>
#include <boost/asio.hpp>
#include <boost/asio/deadline_timer.hpp>
#include "const_vars.h"
#include "router.h"
using boost::asio::ip::tcp;

namespace rest_rpc {
namespace rpc_service {
class connection : public std::enable_shared_from_this<connection>, private boost::noncopyable {
 public:
  connection(boost::asio::io_service& io_service, std::size_t timeout_seconds)
      : socket_(io_service),
        body_(INIT_BUF_SIZE),
        timer_(io_service),
        timeout_seconds_(timeout_seconds),
        has_closed_(false) {}

  ~connection() { close(); }

  void start() { read_head(); }

  tcp::socket& socket() { return socket_; }

  bool has_closed() const { return has_closed_; }

  void response(const char* data, size_t len) {
	std::array<boost::asio::const_buffer, 3> write_buffers;
	write_buffers[0] = boost::asio::buffer(&len, sizeof(uint32_t));
	write_buffers[1] = boost::asio::buffer(&req_id_, sizeof(uint64_t));
	write_buffers[2] = boost::asio::buffer((char*)data, len);
    reset_timer();
    auto self = this->shared_from_this();
    boost::asio::async_write(
        socket_, write_buffers,
        [this, self](boost::system::error_code ec, std::size_t length) {
          cancel_timer();
          if (has_closed()) { return; }
          if (!ec) {
            read_head();
          } else {
            //LOG(INFO) << ec.message();
          }
        });
  }

  void set_conn_id(int64_t id) { conn_id_ = id; }

  int64_t conn_id() const { return conn_id_; }

 private:
  void read_head() {
    reset_timer();
    auto self(this->shared_from_this());
    boost::asio::async_read(
        socket_, boost::asio::buffer(head_),
        [this, self](boost::system::error_code ec, std::size_t length) {
          if (!socket_.is_open()) {
            //LOG(INFO) << "socket already closed";
            return;
          }

          if (!ec) {
            const int body_len = *((int*)(head_));
			req_id_ = *((std::uint64_t*)(head_+sizeof(int32_t)));
            if (body_len > 0 && body_len < MAX_BUF_LEN) {
              if (body_.size() < body_len) { body_.resize(body_len); }
              read_body(body_len);
              return;
            }

            if (body_len == 0) {  // nobody, just head, maybe as heartbeat.
              cancel_timer();
              read_head();
            } else {
              //LOG(INFO) << "invalid body len";
              close();
            }
          } else {
            //LOG(INFO) << ec.message();
            close();
          }
        });
  }

  void read_body(std::size_t size) {
    auto self(this->shared_from_this());
    boost::asio::async_read(
        socket_, boost::asio::buffer(body_.data(), size),
        [this, self](boost::system::error_code ec, std::size_t length) {
          cancel_timer();

          if (!socket_.is_open()) {
            //LOG(INFO) << "socket already closed";
            return;
          }

          if (!ec) {
            router& _router = router::get();
            _router.route(body_.data(), length, this);
          } else {
            //LOG(INFO) << ec.message();
          }
        });
  }

  void reset_timer() {
    if (timeout_seconds_ == 0) { return; }

    auto self(this->shared_from_this());
    timer_.expires_from_now(boost::posix_time::seconds((long)timeout_seconds_));
    timer_.async_wait([this, self](const boost::system::error_code& ec) {
      if (has_closed()) { return; }

      if (ec) { return; }

      //LOG(INFO) << "rpc connection timeout";
      close();
    });
  }

  void cancel_timer() {
    if (timeout_seconds_ == 0) { return; }

    timer_.cancel();
  }

  void close() {
    has_closed_ = true;
    if (socket_.is_open()) {
      boost::system::error_code ignored_ec;
      socket_.shutdown(tcp::socket::shutdown_both, ignored_ec);
      socket_.close(ignored_ec);
    }
  }

  tcp::socket socket_;
  char head_[HEAD_LEN];
  std::vector<char> body_;
  std::uint64_t req_id_;

  boost::asio::deadline_timer timer_;
  std::size_t timeout_seconds_;
  int64_t conn_id_ = 0;
  std::atomic_bool has_closed_;
};
}  // namespace rpc_service
}  // namespace rest_rpc

#endif  // REST_RPC_CONNECTION_H_
