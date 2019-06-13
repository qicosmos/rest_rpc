#ifndef REST_RPC_CONNECTION_H_
#define REST_RPC_CONNECTION_H_

#include <iostream>
#include <memory>
#include <array>
#include <deque>
#include "const_vars.h"
#include "router.h"
#include "use_asio.hpp"
using boost::asio::ip::tcp;

namespace rest_rpc {
	namespace rpc_service {
		class connection : public std::enable_shared_from_this<connection>, private asio::noncopyable {
		public:
			connection(boost::asio::io_service& io_service, std::size_t timeout_seconds)
				: socket_(io_service),
				body_(INIT_BUF_SIZE),
				timer_(io_service),
				timeout_seconds_(timeout_seconds),
				has_closed_(false) {
			}

			~connection() { close(); }

			void start() { read_head(); }

			tcp::socket& socket() { return socket_; }

			bool has_closed() const { return has_closed_; }
			uint64_t request_id() const {
				return req_id_;
			}

			void response(uint64_t req_id, std::string data) {
				auto len = data.size();
				assert(len < MAX_BUF_LEN);

				std::unique_lock<std::mutex> lock(write_mtx_);
				write_queue_.emplace_back(std::make_pair(req_id, std::make_shared<std::string>(std::move(data))));
				if (write_queue_.size() > 1) {
					return;
				}

				write();
			}

			template<typename T>
			void pack_and_response(uint64_t req_id, T data) {
				auto result = msgpack_codec::pack_args_str(result_code::OK, std::move(data));
				response(req_id, std::move(result));
			}

			void close() {
				has_closed_ = true;
				if (socket_.is_open()) {
					boost::system::error_code ignored_ec;
					socket_.shutdown(tcp::socket::shutdown_both, ignored_ec);
					socket_.close(ignored_ec);
				}
			}

			void set_conn_id(int64_t id) { conn_id_ = id; }

			int64_t conn_id() const { return conn_id_; }

			const std::vector<char>& body() const {
				return body_;
			}

			std::string remote_address() const {
				return socket_.remote_endpoint().address().to_string();
			}

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
						const uint32_t body_len = *((int*)(head_));
						req_id_ = *((std::uint64_t*)(head_ + sizeof(int32_t)));
						if (body_len > 0 && body_len < MAX_BUF_LEN) {
							if (body_.size() < body_len) { body_.resize(body_len); }
							read_body(body_len);
							return;
						}

						if (body_len == 0) {  // nobody, just head, maybe as heartbeat.
							cancel_timer();
							read_head();
						}
						else {
							//LOG(INFO) << "invalid body len";
							close();
						}
					}
					else {
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
						read_head();
						router& _router = router::get();
						_router.route<connection>(body_.data(), length, this->shared_from_this());
					}
					else {
						//LOG(INFO) << ec.message();
					}
				});
			}

			void write() {
				auto& pair = write_queue_.front();
				write_size_ = (uint32_t)pair.second->size();
				std::array<boost::asio::const_buffer, 3> write_buffers;
				write_buffers[0] = boost::asio::buffer(&write_size_, sizeof(uint32_t));
				write_buffers[1] = boost::asio::buffer(&pair.first, sizeof(uint64_t));
				write_buffers[2] = boost::asio::buffer(pair.second->data(), write_size_);

				auto self = this->shared_from_this();
				boost::asio::async_write(
					socket_, write_buffers,
					[this, self](boost::system::error_code ec, std::size_t length) {
					on_write(ec, length);
				});
			}

			void on_write(boost::system::error_code ec, std::size_t length) {
				if (ec) {
					std::cout << ec.value() << " " << ec.message() << std::endl;
					close();
					return;
				}

				if (has_closed()) { return; }

				std::unique_lock<std::mutex> lock(write_mtx_);
				write_queue_.pop_front();

				if (!write_queue_.empty()) {
					write();
				}
			}

			void reset_timer() {
				if (timeout_seconds_ == 0) { return; }

				auto self(this->shared_from_this());
				timer_.expires_from_now(std::chrono::seconds(timeout_seconds_));
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

			tcp::socket socket_;
			char head_[HEAD_LEN];
			std::vector<char> body_;
			std::uint64_t req_id_;

			std::deque<std::pair<uint64_t, std::shared_ptr<std::string>>> write_queue_;
			uint32_t write_size_ = 0;
			std::mutex write_mtx_;

			asio::steady_timer timer_;
			std::size_t timeout_seconds_;
			int64_t conn_id_ = 0;
			bool has_closed_;
		};
	}  // namespace rpc_service
}  // namespace rest_rpc

#endif  // REST_RPC_CONNECTION_H_
