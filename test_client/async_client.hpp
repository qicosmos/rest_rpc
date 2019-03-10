#pragma once
#include <string>
#include <deque>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/noncopyable.hpp>
using boost::asio::ip::tcp;
#include "../codec.h"
#include "../const_vars.h"
using namespace rest_rpc;
using namespace rest_rpc::rpc_service;

class async_client : private boost::noncopyable {
public:
	async_client(const std::string& host, unsigned short port) : socket_(ios_), work_(ios_), strand_(ios_),
		deadline_(ios_), host_(host), port_(port), body_(PAGE_SIZE){
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
				std::cout << "connect "<< host_<<" "<<port_ << std::endl;
			}
		});
	}

	template<typename T = void, typename... Args>
	typename std::enable_if<std::is_void<T>::value>::type call(std::string rpc_name, Args&&... args) {
		msgpack_codec codec;
		auto ret = codec.pack_args(std::move(rpc_name), std::forward<Args>(args)...);
		write(std::move(ret));
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
	void reset_deadline_timer(size_t timeout) {
		deadline_.expires_from_now(boost::posix_time::seconds((long)timeout));
		deadline_.async_wait([this](const boost::system::error_code& ec) {
			if (!ec) {
				socket_.close();
			}
		});
	}

	void write(buffer_type&& message) {
		size_t size = message.size();
		std::string_view s(message.release(), size);
		strand_.post([this, msg = std::move(s)]{
			outbox_.emplace_back(std::move(msg));
			if (outbox_.size() > 1) {
				// outstanding async_write
				return;
			}

			this->write();
			});
	}

	void write(){
		auto msg = outbox_[0];
		size_t size = msg.length();
		write_buffers_[0] = boost::asio::buffer(&size, sizeof(int32_t));
		write_buffers_[1] = boost::asio::buffer((char*)msg.data(), size);
		boost::asio::async_write(socket_, write_buffers_,
			strand_.wrap([this](const boost::system::error_code& ec, const size_t length) {
				::free((char*)outbox_.front().data());
				outbox_.pop_front();

				if (ec) {
					std::cerr << "could not write: " << ec.message() << std::endl;
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
		boost::asio::async_read(socket_, read_buffers_, boost::asio::transfer_at_least(HEAD_LEN),
			[this](const boost::system::error_code& ec, const size_t length) {
			if (!socket_.is_open()) {
				//LOG(INFO) << "socket already closed";
				return;
			}

			if (!ec) {
				const int body_len = *((int*)(head_));
				if (body_len < 0) {
					//LOG(INFO) << "invalid body len";
					close();
					return;
				}

				if (body_len == 0) {  // nobody, just head, maybe as heartbeat.
					//cancel_timer();
					do_read();
					return;
				}
				
				if (body_len > MAX_BUF_LEN) {
					//that depends, the user decide to close or continue read
					//LOG(INFO)<<body len is greater than MAX_BUF_LEN
					return;
				}

				if (body_len <= body_.size()) {
					//entire body: (body_, body_len)
					do_read();
				}
				else {
					body_.resize(body_len);
					read_body(length - HEAD_LEN, body_len);
				}
			}
			else {
				//LOG(INFO) << ec.message();
				close();
			}
		});
	}

	void read_body(size_t start, size_t body_len) {
		boost::asio::async_read(
			socket_, boost::asio::buffer(body_.data()+ start, body_len - start),
			[this](boost::system::error_code ec, std::size_t length) {
			//cancel_timer();

			if (!socket_.is_open()) {
				//LOG(INFO) << "socket already closed";
				return;
			}

			if (!ec) {
				//entier body
				std::cout << length << std::endl;
				do_read();
			}
			else {
				//LOG(INFO) << ec.message();
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
	size_t connect_timeout_  = 1;//s
	int reconnect_cnt_ = -1;

	boost::asio::deadline_timer deadline_;

	std::deque<std::string_view> outbox_;

	std::array<boost::asio::const_buffer, 2> write_buffers_;

	char head_[HEAD_LEN] = {};
	std::vector<char> body_;
	std::array<boost::asio::mutable_buffer, 2> read_buffers_;
};