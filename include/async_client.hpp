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
		req_result(std::string_view data): data_(data) {}
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

		void set_wait_timeout(size_t seconds) {
			wait_timeout_ = seconds;
		}

		void connect() {
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

		void set_error_callback(std::function<void(boost::system::error_code)> f) {
			err_cb_ = std::move(f);
		}

		//sync call
		template<typename T=void, typename... Args>
		auto call(const std::string& rpc_name, Args&&... args) {
			std::future<req_result> future = async_call(rpc_name, std::forward<Args>(args)...);
			if (future.wait_for(std::chrono::seconds(wait_timeout_)) == std::future_status::timeout) {
				throw std::out_of_range("timeout");
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
			constexpr const size_t SIZE = sizeof...(Args);
			if constexpr (sizeof...(Args) > 0) {
				using last_type = last_type_of<Args...>;
				if constexpr (std::is_invocable_v<last_type, boost::system::error_code, std::string_view>) {
					call_with_cb(rpc_name, std::make_index_sequence<SIZE - 1>{},
						std::forward_as_tuple(std::forward<Args>(args)...));
				}
				else {
					if constexpr (sizeof...(Args) > 1){
						if constexpr (std::is_invocable_v<nth_type_of<SIZE - 2, Args...>, boost::system::error_code, std::string_view>) {
							call_with_cb<true>(rpc_name, std::make_index_sequence<SIZE - 2>{},
								std::forward_as_tuple(std::forward<Args>(args)...));
						}
						else {
							auto future = get_future();
							call_impl(rpc_name, std::forward<Args>(args)...);
							return future;
						}
					}
					else {
						auto future = get_future();
						call_impl(rpc_name, std::forward<Args>(args)...);
						return future;
					}
				}
			}
			else {
				auto future = get_future();
				call_impl(rpc_name, std::forward<Args>(args)...);
				return future;
			}
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
		class call_t {
		public:
			call_t(boost::asio::io_service& ios, async_client* client,
				uint64_t req_id, std::function<void(boost::system::error_code, std::string_view)> user_callback,
				size_t timeout = 3000) : timer_(ios), client_(client), req_id_(req_id), user_callback_(std::move(user_callback)) {
				timer_.expires_from_now(std::chrono::milliseconds(timeout));
				timer_.async_wait([this](boost::system::error_code ec) {
					if (ec) {
						return;
					}

					has_timeout_ = true;
					user_callback_(errc::make_error_code(errc::timed_out), {});
					client_->erase(req_id_);
				});
			}

			void callback(boost::system::error_code ec, std::string_view data) {
				user_callback_(ec, data);
			}

			bool has_timeout() const {
				return has_timeout_;
			}

			void cancel() {
				boost::system::error_code ec;
				timer_.cancel(ec);
			}

			~call_t() {
				cancel();
			}

		private:
			boost::asio::steady_timer timer_;
			async_client* client_;
			uint64_t req_id_;
			std::function<void(boost::system::error_code, std::string_view)> user_callback_;
			bool has_timeout_ = false;
		};

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
					auto req_id = *((std::uint64_t*)(head_ + sizeof(int32_t)));

					if (body_len == 0 || body_len > MAX_BUF_LEN) {
						//LOG(INFO) << "invalid body len";
						call_back(req_id, errc::make_error_code(errc::invalid_argument), {});
						close();
						return;
					}

					if (body_len > body_.size()) {
						body_.resize(body_len);
						read_buffers_[1] = boost::asio::buffer(body_);
						read_body(req_id, length - HEAD_LEN, body_len);
						return;
					}

					call_back(req_id, ec, { body_.data(), body_len });

					do_read();
				}
				else {
					//LOG(INFO) << ec.message();
					if (err_cb_) err_cb_(ec);
					close();
				}
			});
		}

		template<typename... Args>
		void call_impl(const std::string& rpc_name, Args&&... args) {
			msgpack_codec codec;
			auto ret = codec.pack_args(rpc_name, std::forward<Args>(args)...);
			write(req_id_, std::move(ret));
		}

		template<bool has_timeout = false, size_t... idx, typename Tuple>
		void call_with_cb(const std::string& rpc_name, std::index_sequence<idx...>, Tuple&& tp) {
			if constexpr (has_timeout) {
				cb_map_.emplace(req_id_, std::make_unique<call_t>(ios_, this,
					req_id_, std::move(std::get<sizeof...(idx)>(std::forward<Tuple>(tp))),
					std::get<sizeof...(idx) + 1>(std::forward<Tuple>(tp))));
			}
			else {
				cb_map_.emplace(req_id_, std::make_unique<call_t>(ios_, this,
					req_id_, std::move(std::get<sizeof...(idx)>(std::forward<Tuple>(tp)))));
			}

			call_impl(rpc_name, std::get<idx>(std::forward<Tuple>(tp))...);
		}

		std::future<req_result> get_future() {
			auto p = std::promise<req_result>();
			auto future = p.get_future();
			future_map_.emplace(req_id_, std::move(p));
			return future;
		}

		void call_back(uint64_t req_id, const boost::system::error_code& ec, std::string_view data) {
			if (!cb_map_.empty()) {
				auto& call_helper = cb_map_[req_id];
				if (call_helper && !call_helper->has_timeout()) {
					call_helper->cancel();
					call_helper->callback(ec, data);
				}

				erase(req_id);
			}
			else {
				auto& f = future_map_[req_id];
				f.set_value(req_result{data});
				strand_.post([this, req_id]() { future_map_.erase(req_id); });
			}
		}

		void erase(uint64_t req_id) {
			strand_.post([this, req_id]() { cb_map_.erase(req_id); });
		}

		void read_body(std::uint64_t req_id, size_t start, size_t body_len) {
			boost::asio::async_read(
				socket_, boost::asio::buffer(body_.data() + start, body_len - start),
				[this, req_id, body_len](boost::system::error_code ec, std::size_t length) {
				//cancel_timer();

				if (!socket_.is_open()) {
					//LOG(INFO) << "socket already closed";
					call_back(req_id, errc::make_error_code(errc::connection_aborted), {});
					return;
				}

				if (!ec) {
					//entier body
					std::cout << length << std::endl;
					call_back(req_id, ec, {body_.data(), body_len });

					do_read();
				}
				else {
					//LOG(INFO) << ec.message();
					call_back(req_id, ec, {});
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
		size_t connect_timeout_ = 2;//s
		size_t wait_timeout_ = 2;//s
		int reconnect_cnt_ = -1;

		boost::asio::deadline_timer deadline_;

		std::deque<message_type> outbox_;
		uint64_t req_id_ = 0;
		std::function<void(boost::system::error_code)> err_cb_;

		std::unordered_map<std::uint64_t, std::unique_ptr<call_t>> cb_map_;
		std::unordered_map<std::uint64_t, std::promise<req_result>> future_map_;

		char head_[HEAD_LEN] = {};
		std::vector<char> body_;
		std::array<boost::asio::mutable_buffer, 2> read_buffers_;
	};
}