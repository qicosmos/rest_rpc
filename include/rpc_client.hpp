#pragma once
#include <string>
#include <boost/asio.hpp>
#include <boost/noncopyable.hpp>
using boost::asio::ip::tcp;
#include "codec.h"

using namespace rest_rpc::rpc_service;

namespace rest_rpc {
	class sync_client : private boost::noncopyable
	{
	public:
		sync_client(boost::asio::io_service& io_service)
			: io_service_(io_service),
			socket_(io_service) {}

		void connect(const std::string& addr, const std::string& port) {
			tcp::resolver resolver(io_service_);
			tcp::resolver::query query(tcp::v4(), addr, port);
			tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);

			boost::asio::connect(socket_, endpoint_iterator);
		}

		std::string call(const char* data, size_t size) {
			bool r = send(data, size);
			if (!r) {
				throw std::runtime_error("call failed");
			}

			socket_.receive(boost::asio::buffer(&size, 4));
			std::string recv_data;
			recv_data.resize(size);
			socket_.receive(boost::asio::buffer(&recv_data[0], size));
			return recv_data;
		}

		std::string call(std::string content) {
			return call(content.data(), content.size());
		}

		template<typename T = void, typename... Args>
		typename std::enable_if<std::is_void<T>::value>::type call(std::string rpc_name, Args... args) {
			msgpack_codec codec;
			auto ret = codec.pack_args(rpc_name, args...);

			auto result = call(ret.data(), ret.size());
			auto tp = codec.unpack<std::tuple<int>>(result.data(), result.size());
			if (std::get<0>(tp) != 0) {
				auto err_tp = codec.unpack<std::tuple<int, std::string>>(result.data(), result.size());
				throw std::logic_error(std::get<1>(err_tp));
			}
		}

		template<typename T, typename... Args>
		typename std::enable_if<!std::is_void<T>::value, T>::type call(std::string rpc_name, Args... args) {
			msgpack_codec codec;
			auto ret = codec.pack_args(rpc_name, args...);

			auto result = call(ret.data(), ret.size());
			auto tp = codec.unpack<std::tuple<int>>(result.data(), result.size());
			if (std::get<0>(tp) != 0) {
				auto err_tp = codec.unpack<std::tuple<int, std::string>>(result.data(), result.size());
				throw std::logic_error(std::get<1>(err_tp));
			}
			else {
				auto err_tp = codec.unpack<std::tuple<int, T>>(result.data(), result.size());
				return std::get<1>(err_tp);
			}
		}

	private:
		bool send(const char* data, size_t size) {
			std::vector<boost::asio::const_buffer> message;
			message.push_back(boost::asio::buffer(&size, 4));
			message.push_back(boost::asio::buffer(data, size));
			boost::system::error_code ec;
			boost::asio::write(socket_, message, ec);
			if (ec) {
				return false;
			}
			else {
				return true;
			}
		}

		boost::asio::io_service& io_service_;
		tcp::socket socket_;
	};
}

