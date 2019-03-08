#pragma once
#include <string>
#include <deque>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
using boost::asio::ip::tcp;
#include "../codec.h"
using namespace rest_rpc;
using namespace rest_rpc::rpc_service;

class async_client : private boost::noncopyable {
public:
	async_client(const std::string& host, unsigned short port) : socket_(ios_), work_(ios_), strand_(ios_),
		deadline_(ios_), host_(host), port_(port) {
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

	void write(std::string&& message)
	{
		strand_.post([this, msg = std::move(message)] {
			outbox_.emplace_back(std::move(msg));
			if (outbox_.size() > 1) {
				// outstanding async_write
				return;
			}

			this->write();
		});
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

	void write()
	{
		const std::string& message = outbox_[0];
		boost::asio::async_write(socket_,boost::asio::buffer(message.c_str(), message.size()),
			strand_.wrap([this](const boost::system::error_code& ec, const size_t bytesTransferred) {
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

	std::deque<std::string> outbox_;
};