#pragma once
#include <string>
#include <boost/asio.hpp>
using boost::asio::ip::tcp;
#include "../codec.h"
using namespace rest_rpc;
using namespace rest_rpc::rpc_service;

class async_client : private boost::noncopyable {
public:
	async_client(const std::string& host, unsigned short port) : socket_(ios_), work_(ios_), 
		connect_timer_(ios_), host_(host), port_(port) {
	}

	void set_timeout(int milliseconds) {
		assert(milliseconds > 0);
		timeout_ = milliseconds;
	}

	void set_reconnect_count(int reconnect_count) {
		reconnect_cnt_ = reconnect_count;
	}

	void connect() {
		reset_connect_timer();
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
				std::cout << "connect "<< host_<<" "<<port_ << std::endl;
			}
		});
	}

	void run() {
		ios_.run();
	}

	bool has_connected() const {
		return has_connected_;
	}

private:
	void reset_connect_timer() {
		connect_timer_.expires_from_now(boost::posix_time::milliseconds(timeout_));
		connect_timer_.async_wait([this](const boost::system::error_code& ec) {
			if (!ec) {
				socket_.close();
			}
		});
	}

	bool has_connected_ = false;
	boost::asio::io_service ios_;
	tcp::socket socket_;
	boost::asio::io_service::work work_;

	std::string host_;
	unsigned short port_;
	int timeout_=1000;//ms
	int reconnect_cnt_ = -1;

	boost::asio::deadline_timer connect_timer_;
};