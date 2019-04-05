#pragma once
#include <string>
#include <deque>
#include <future>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/noncopyable.hpp>
using boost::asio::ip::tcp;
#include "client_util.hpp"
#include "const_vars.h"
#include "meta_util.hpp"

using namespace rest_rpc::rpc_service;

namespace rest_rpc {
	using namespace boost::system;
	class req_result {
	public:
		req_result() = default;
		req_result(std::string_view data) : data_(data) {}
		bool success() const {
			return !has_error(data_);
		}

		template<typename T>
		T as() {
			return get_result<T>(data_);
		}

		void as() {
			if (has_error(data_)) {
				throw std::logic_error("rpc error");
			}
		}
	private:
		std::string_view data_;
	};

	class rpc_client : private boost::noncopyable {
	public:
		rpc_client(const std::string& host, unsigned short port) : socket_(ios_), work_(ios_), strand_(ios_),
			deadline_(ios_), host_(host), port_(port), body_(INIT_BUF_SIZE) {
			thd_ = std::make_shared<std::thread>([this] {
				ios_.run();
			});
		}

		~rpc_client() {
			stop();
		}

		void set_connect_timeout(size_t seconds) {
			connect_timeout_ = seconds;
		}

		void set_reconnect_count(int reconnect_count) {
			reconnect_cnt_ = reconnect_count;
		}

		void set_wait_timeout(size_t seconds) {
			wait_timeout_ = seconds;
		}

		void async_connect() {
			reset_deadline_timer(connect_timeout_);
			auto addr = boost::asio::ip::address::from_string(host_);
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
					async_connect();
				}
				else {
					has_connected_ = true;
					deadline_.cancel();
					do_read();
					conn_cond_.notify_one();
				}
			});
		}

		bool connect(size_t timeout = 1) {
			async_connect();
			return wait_conn(timeout);
		}

		bool wait_conn(size_t timeout) {
			if (has_connected_) {
				return true;
			}

			std::unique_lock lock(conn_mtx_);
			bool result = conn_cond_.wait_for(lock, std::chrono::seconds(timeout),
				[this] {return has_connected_; });
			return result;
		}

		void set_error_callback(std::function<void(boost::system::error_code)> f) {
			err_cb_ = std::move(f);
		}

		//sync call
		template<typename T = void, typename... Args>
		auto call(const std::string& rpc_name, Args&&... args) {
			std::future<req_result> future = async_call(rpc_name, std::forward<Args>(args)...);
			auto status = future.wait_for(std::chrono::seconds(1));
			if (status == std::future_status::timeout || status == std::future_status::deferred) {
				throw std::out_of_range("timeout or deferred");
			}

			if constexpr (std::is_void_v<T>) {
				future.get().as();
			}
			else {
				return future.get().as<T>();
			}
		}

		template<typename... Args>
		auto async_call(const std::string& rpc_name, Args&&... args) {
			req_id_++;
			auto future = get_future();
			msgpack_codec codec;
			auto ret = codec.pack_args(rpc_name, std::forward<Args>(args)...);
			write(req_id_, std::move(ret));
			return future;
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
			assert(size < MAX_BUF_LEN);
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
			boost::asio::async_read(socket_, boost::asio::buffer(head_),
				[this](const boost::system::error_code& ec, const size_t length) {
				if (!socket_.is_open()) {
					//LOG(INFO) << "socket already closed";
					if (err_cb_) err_cb_(errc::make_error_code(errc::connection_aborted));
					return;
				}

				if (!ec) {
					const uint32_t body_len = *((uint32_t*)(head_));
					auto req_id = *((std::uint64_t*)(head_ + sizeof(int32_t)));
					if (body_len > 0 && body_len < MAX_BUF_LEN) {
						if (body_.size() < body_len) { body_.resize(body_len); }
						read_body(req_id, body_len);
						return;
					}

					if (body_len == 0 || body_len > MAX_BUF_LEN) {
						//LOG(INFO) << "invalid body len";
						call_back(req_id, errc::make_error_code(errc::invalid_argument), {});
						close();
						return;
					}
				}
				else {
					//LOG(INFO) << ec.message();
					if (err_cb_) err_cb_(ec);
					close();
				}
			});
		}

		void read_body(std::uint64_t req_id, size_t body_len) {
			boost::asio::async_read(
				socket_, boost::asio::buffer(body_.data(), body_len),
				[this, req_id, body_len](boost::system::error_code ec, std::size_t length) {
				//cancel_timer();

				if (!socket_.is_open()) {
					//LOG(INFO) << "socket already closed";
					call_back(req_id, errc::make_error_code(errc::connection_aborted), {});
					return;
				}

				if (!ec) {
					//entier body
					call_back(req_id, ec, { body_.data(), body_len });

					do_read();
				}
				else {
					//LOG(INFO) << ec.message();
					call_back(req_id, ec, {});
				}
			});
		}

		std::future<req_result> get_future() {
			auto p = std::make_shared<std::promise<req_result>>();
			
			std::future future = p->get_future();
			strand_.post([this, p1 = std::move(p)] () mutable { 
				future_map_.emplace(req_id_, std::move(p1)); 
			});
			return future;
		}

		void call_back(uint64_t req_id, const boost::system::error_code& ec, std::string_view data) {
			if (ec) {
				//LOG<<ec.message();
			}

			auto& f = future_map_[req_id];
			f->set_value(req_result{ data });
			strand_.post([this, req_id]() {
				future_map_.erase(req_id);
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

		boost::asio::io_service ios_;
		tcp::socket socket_;
		boost::asio::io_service::work work_;
		boost::asio::io_service::strand strand_;
		std::shared_ptr<std::thread> thd_ = nullptr;

		std::string host_;
		unsigned short port_;
		size_t connect_timeout_ = 2;//s
		size_t wait_timeout_ = 2;//s
		int reconnect_cnt_ = -1;
		bool has_connected_ = false;
		std::mutex conn_mtx_;
		std::condition_variable conn_cond_;

		boost::asio::deadline_timer deadline_;

		std::deque<message_type> outbox_;
		uint64_t req_id_ = 0;
		std::function<void(boost::system::error_code)> err_cb_;

		std::unordered_map<std::uint64_t, std::shared_ptr<std::promise<req_result>>> future_map_;

		char head_[HEAD_LEN] = {};
		std::vector<char> body_;
	};
}