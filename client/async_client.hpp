#pragma once
#include <string>
#include <deque>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/noncopyable.hpp>
using boost::asio::ip::tcp;
#include "client_util.hpp"
#include "../const_vars.h"

using namespace rest_rpc::rpc_service;

namespace rest_rpc {
	class async_client : private boost::noncopyable {
	public:
		async_client(const std::string& host, unsigned short port) : socket_(ios_), work_(ios_), strand_(ios_),
			deadline_(ios_), host_(host), port_(port), body_(INIT_BUF_SIZE) {
			read_buffers_[0] = boost::asio::buffer(head_);
			read_buffers_[1] = boost::asio::buffer(body_);
			thd_ = std::make_shared<std::thread>([this] {
				ios_.run();
			});
		}

		~async_client() {
			stop();
		}

		void set_connect_timeout(size_t seconds) {
			connect_timeout_ = seconds;
		}

		void set_reconnect_count(int reconnect_count) {
			reconnect_cnt_ = reconnect_count;
		}

		void connect() {
			reset_deadline_timer(connect_timeout_);
			auto addr = boost::asio::ip::make_address_v4(host_);
			socket_.async_connect({ addr, port_ }, [this](const boost::system::error_code& ec) {
				if (ec) {
					std::cout << ec.message() << std::endl;
					has_connected_ = false;
					if (reconnect_cnt_ == 0) {
						return;
					}

					if (reconnect_cnt_ > 0) {
						reconnect_cnt_--;
					}

					socket_ = decltype(socket_)(ios_);
					connect();
				}
				else {
					has_connected_ = true;
					deadline_.cancel();
					do_read();
					std::cout << "connect " << host_ << " " << port_ << std::endl;
				}
			});
		}

		template<typename F>
		void set_callback(const std::string& rpc_name, F f) {
			cb_map_[std::hash<std::string>{}(rpc_name)] = std::move(f);
		}

		void set_error_callback(std::function<void(boost::system::error_code)> f) {
			err_cb_ = std::move(f);
		}

		template<typename... Args>
		void call(const std::string& rpc_name, Args&&... args) {
			std::uint64_t req_id = std::hash<std::string>{}(rpc_name);
			msgpack_codec codec;
			auto ret = codec.pack_args(rpc_name, std::forward<Args>(args)...);
			write(req_id, std::move(ret));
		}

		bool has_connected() const {
			return has_connected_;
		}

		void stop() {
			if (thd_ != nullptr) {
				ios_.stop();
				thd_->join();
				thd_ = nullptr;
			}
		}

	private:
		using message_type = std::pair<std::string_view, std::uint64_t>;
		void reset_deadline_timer(size_t timeout) {
			deadline_.expires_from_now(boost::posix_time::seconds((long)timeout));
			deadline_.async_wait([this](const boost::system::error_code& ec) {
				if (!ec) {
					socket_.close();
				}
			});
		}

		void write(std::uint64_t req_id, buffer_type&& message) {
			size_t size = message.size();
			message_type msg{ {message.release(), size}, req_id };
			strand_.post([this, msg] {
				outbox_.emplace_back(std::move(msg));
				if (outbox_.size() > 1) {
					// outstanding async_write
					return;
				}

				this->write();
			});
		}

		void write() {
			auto& msg = outbox_[0];
			size_t write_len = msg.first.length();
			std::array<boost::asio::const_buffer, 3> write_buffers;
			write_buffers[0] = boost::asio::buffer(&write_len, sizeof(int32_t));
			write_buffers[1] = boost::asio::buffer(&msg.second, sizeof(uint64_t));
			write_buffers[2] = boost::asio::buffer((char*)msg.first.data(), write_len);
			boost::asio::async_write(socket_, write_buffers,
				strand_.wrap([this](const boost::system::error_code& ec, const size_t length) {
				::free((char*)outbox_.front().first.data());
				outbox_.pop_front();

				if (ec) {
					if (err_cb_) err_cb_(ec);
					return;
				}

				if (!outbox_.empty()) {
					// more messages to send
					this->write();
				}
			})
			);
		}

		void do_read() {
			using namespace boost::system;
			std::array<boost::asio::mutable_buffer, 2> read_buffers;
			read_buffers[0] = boost::asio::buffer(head_);
			read_buffers[1] = boost::asio::buffer(body_);

			boost::asio::async_read(socket_, read_buffers, boost::asio::transfer_at_least(HEAD_LEN),
				[this](const boost::system::error_code& ec, const size_t length) {
				if (!socket_.is_open()) {
					//LOG(INFO) << "socket already closed";
					if (err_cb_) err_cb_(errc::make_error_code(errc::connection_aborted));
					return;
				}

				if (!ec) {
					const uint32_t body_len = *((uint32_t*)(head_));
					if (body_len == 0 || body_len > MAX_BUF_LEN) {
						//LOG(INFO) << "invalid body len";
						if (err_cb_) err_cb_(errc::make_error_code(errc::invalid_argument));
						close();
						return;
					}

					auto req_id = *((std::uint64_t*)(head_ + sizeof(int32_t)));

					if (body_len > body_.size()) {
						body_.resize(body_len);
						read_body(req_id, length - HEAD_LEN, body_len);
						return;
					}

					auto& callback = cb_map_[req_id];
					if (callback) {
						callback({ body_.data(), body_len });
					}

					do_read();
				}
				else {
					//LOG(INFO) << ec.message();
					if (err_cb_) err_cb_(ec);
					close();
				}
			});
		}

		void read_body(std::uint64_t req_id, size_t start, size_t body_len) {
			using namespace boost::system;
			boost::asio::async_read(
				socket_, boost::asio::buffer(body_.data() + start, body_len - start),
				[this, req_id](boost::system::error_code ec, std::size_t length) {
				//cancel_timer();

				if (!socket_.is_open()) {
					//LOG(INFO) << "socket already closed";
					if (err_cb_) err_cb_(errc::make_error_code(errc::connection_aborted));
					return;
				}

				if (!ec) {
					//entier body
					std::cout << length << std::endl;
					auto& callback = cb_map_[req_id];
					if (callback) {
						callback({ body_.data(), body_.size() });
					}

					do_read();
				}
				else {
					//LOG(INFO) << ec.message();
					if (err_cb_) err_cb_(ec);
				}
			});
		}

		void close() {
			has_connected_ = true;
			if (socket_.is_open()) {
				boost::system::error_code ignored_ec;
				socket_.shutdown(tcp::socket::shutdown_both, ignored_ec);
				socket_.close(ignored_ec);
			}
		}

		bool has_connected_ = false;
		boost::asio::io_service ios_;
		tcp::socket socket_;
		boost::asio::io_service::work work_;
		boost::asio::io_service::strand strand_;
		std::shared_ptr<std::thread> thd_ = nullptr;

		std::string host_;
		unsigned short port_;
		size_t connect_timeout_ = 1;//s
		int reconnect_cnt_ = -1;

		boost::asio::deadline_timer deadline_;

		std::deque<message_type> outbox_;
		std::unordered_map<std::uint64_t, std::function<void(std::string_view)>> cb_map_;
		std::function<void(boost::system::error_code)> err_cb_;

		char head_[HEAD_LEN] = {};
		std::vector<char> body_;
		std::array<boost::asio::mutable_buffer, 2> read_buffers_;
	};
}