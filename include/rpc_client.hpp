#pragma once
#include <iostream>
#include <string>
#include <deque>
#include <future>
#include "use_asio.hpp"
#include "client_util.hpp"
#include "const_vars.h"
#include "meta_util.hpp"

using namespace rest_rpc::rpc_service;

namespace rest_rpc {
	class req_result {
	public:
		req_result() = default;
		req_result(string_view data) : data_(data.data(), data.length()) {}
		bool success() const {
			return !has_error(data_);
		}

		template<typename T>
		T as() {
			if (has_error(data_)) {
				throw std::logic_error(get_error_msg(data_));
			}

			return get_result<T>(data_);
		}

		void as() {
			if (has_error(data_)) {
				throw std::logic_error(get_error_msg(data_));
			}
		}

	private:
		std::string data_;
	};

	enum class CallModel {
		future,
		callback
	};
	const constexpr auto FUTURE = CallModel::future;

	const constexpr size_t DEFAULT_TIMEOUT = 5000; //milliseconds

	class rpc_client : private asio::noncopyable {
	public:
		rpc_client() : socket_(ios_), work_(ios_), 
			deadline_(ios_), body_(INIT_BUF_SIZE) {
			thd_ = std::make_shared<std::thread>([this] {
				ios_.run();
			});
		}

		rpc_client(const std::string& host, unsigned short port) : socket_(ios_), work_(ios_), 
			deadline_(ios_), host_(host), port_(port), body_(INIT_BUF_SIZE) {
			thd_ = std::make_shared<std::thread>([this] {
				ios_.run();
			});
		}

		~rpc_client() {            
			close();
            stop();
		}

		void run(){
            thd_->join();
		}

		void set_connect_timeout(size_t milliseconds) {
			connect_timeout_ = milliseconds;
		}

		void set_reconnect_count(int reconnect_count) {
			reconnect_cnt_ = reconnect_count;
		}

		bool connect(size_t timeout = 3, bool is_ssl = false) {
			if (has_connected_)
				return true;

			assert(port_ != 0);
            if (is_ssl) {
                upgrade_to_ssl();
            }
			async_connect();
			return wait_conn(timeout);
		}

		bool connect(const std::string& host, unsigned short port, bool is_ssl = false, size_t timeout = 3) {
			if (port_==0) {
				host_ = host;
				port_ = port;
			}

			return connect(timeout, is_ssl);
		}

		void async_connect(const std::string& host, unsigned short port) {
			if (port_ == 0) {
				host_ = host;
				port_ = port;
			}

			async_connect();
		}

		bool wait_conn(size_t timeout) {
			if (has_connected_) {
				return true;
			}

            has_wait_ = true;
			std::unique_lock<std::mutex> lock(conn_mtx_);
			bool result = conn_cond_.wait_for(lock, std::chrono::seconds(timeout),
				[this] {return has_connected_.load(); });
            has_wait_ = false;
			return has_connected_;
		}

		void enable_auto_reconnect(bool enable = true) {
			enable_reconnect_ = enable;
		}

		void enable_auto_heartbeat(bool enable = true) {
			if (enable) {
				reset_deadline_timer(5);
			}
			else {
				deadline_.cancel();
			}				
		}

		void update_addr(const std::string& host, unsigned short port) {
			host_ = host;
			port_ = port;
		}

        void close(bool close_ssl = true) {
            boost::system::error_code ec;
            if (close_ssl) {
#ifdef CINATRA_ENABLE_SSL
                if (ssl_stream_) {
                    ssl_stream_->shutdown(ec);
                    ssl_stream_ = nullptr;
                }
#endif
            }

            if (!has_connected_)
                return;

            has_connected_ = false;            
            socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            socket_.close(ec);
            clear_cache();
        }

		void set_error_callback(std::function<void(boost::system::error_code)> f) {
			err_cb_ = std::move(f);
		}

		uint64_t reqest_id() {
			return temp_req_id_;
		}

		bool has_connected() const {
			return has_connected_;
		}

		//sync call
#if __cplusplus > 201402L
		template<size_t TIMEOUT, typename T = void, typename... Args>
		auto call(const std::string& rpc_name, Args&& ... args) {
			std::future<req_result> future = async_call<FUTURE>(rpc_name, std::forward<Args>(args)...);
			auto status = future.wait_for(std::chrono::milliseconds(TIMEOUT));
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

		template<typename T = void, typename... Args>
		auto call(const std::string& rpc_name, Args&& ... args) {
			return call<DEFAULT_TIMEOUT, T>(rpc_name, std::forward<Args>(args)...);
		}
#else
		template<size_t TIMEOUT, typename T=void, typename... Args>
		typename std::enable_if<std::is_void<T>::value>::type call(const std::string& rpc_name, Args&& ... args) {
			std::future<req_result> future = async_call<FUTURE>(rpc_name, std::forward<Args>(args)...);
			auto status = future.wait_for(std::chrono::milliseconds(TIMEOUT));
			if (status == std::future_status::timeout || status == std::future_status::deferred) {
				throw std::out_of_range("timeout or deferred");
			}

			future.get().as();
		}

		template<typename T = void, typename... Args>
		typename std::enable_if<std::is_void<T>::value>::type call(const std::string& rpc_name, Args&& ... args) {
			call<DEFAULT_TIMEOUT, T>(rpc_name, std::forward<Args>(args)...);
		}

		template<size_t TIMEOUT, typename T, typename... Args>
		typename std::enable_if<!std::is_void<T>::value, T>::type call(const std::string& rpc_name, Args&& ... args) {
			std::future<req_result> future = async_call<FUTURE>(rpc_name, std::forward<Args>(args)...);
			auto status = future.wait_for(std::chrono::milliseconds(TIMEOUT));
			if (status == std::future_status::timeout || status == std::future_status::deferred) {
				throw std::out_of_range("timeout or deferred");
			}

			return future.get().as<T>();
		}

		template<typename T, typename... Args>
		typename std::enable_if<!std::is_void<T>::value, T>::type call(const std::string& rpc_name, Args&& ... args) {
			return call<DEFAULT_TIMEOUT, T>(rpc_name, std::forward<Args>(args)...);
		}
#endif

		template<CallModel model, typename... Args>
		std::future<req_result> async_call(const std::string& rpc_name, Args&&... args) {
			auto p = std::make_shared<std::promise<req_result>>();
			std::future<req_result> future = p->get_future();

			uint64_t fu_id = 0;
			{
				std::unique_lock<std::mutex> lock(cb_mtx_);
				fu_id_++;
				fu_id = fu_id_;
				future_map_.emplace(fu_id, std::move(p));
			}

			msgpack_codec codec;
			auto ret = codec.pack_args(rpc_name, std::forward<Args>(args)...);
			write(fu_id, request_type::req_res, std::move(ret));
			return future;
		}

		template<size_t TIMEOUT = DEFAULT_TIMEOUT, typename... Args>
		void async_call(const std::string& rpc_name, std::function<void(boost::system::error_code, string_view)> cb, Args&& ... args) {
			if (!has_connected_) {
                if(cb)
                    cb(boost::asio::error::make_error_code(boost::asio::error::not_connected), "not connected");
				return;
			}

			uint64_t cb_id = 0;
			{
				std::unique_lock<std::mutex> lock(cb_mtx_);
				callback_id_++;
				callback_id_ |= (uint64_t(1) << 63);
				cb_id = callback_id_;
				auto call = std::make_shared<call_t>(ios_, std::move(cb), TIMEOUT);
				call->start_timer();
				callback_map_.emplace(cb_id, call);
			}

			msgpack_codec codec;
			auto ret = codec.pack_args(rpc_name, std::forward<Args>(args)...);
			write(cb_id, request_type::req_res, std::move(ret));
		}

		void stop() {
			if (thd_ != nullptr) {
				ios_.stop();
				thd_->join();
				thd_ = nullptr;
			}
		}

		template<typename Func>
		void subscribe(std::string key, Func f) {
			auto it = sub_map_.find(key);
			if (it != sub_map_.end()) {
				assert("duplicated subscribe");
				return;
			}

			sub_map_.emplace(key, std::move(f));
			send_subscribe(key, "");
			key_token_set_.emplace(std::move(key), "");
		}

		template<typename Func>
		void subscribe(std::string key, std::string token, Func f) {
			auto composite_key = key + token;
			auto it = sub_map_.find(composite_key);
			if (it != sub_map_.end()) {
				assert("duplicated subscribe");
				return;
			}

			sub_map_.emplace(std::move(composite_key), std::move(f));			
			send_subscribe(key, token);
			key_token_set_.emplace(std::move(key), std::move(token));
		}

		template<typename T, size_t TIMEOUT = 3>
		void publish(std::string key, T&& t) {
			msgpack_codec codec;
			auto buf = codec.pack(std::move(t));
			call<TIMEOUT>("publish", std::move(key), "", std::string(buf.data(), buf.size()));
		}

		template<typename T, size_t TIMEOUT=3>
		void publish_by_token(std::string key, std::string token, T&& t) {
			msgpack_codec codec;
			auto buf = codec.pack(std::move(t));
			call<TIMEOUT>("publish_by_token", std::move(key), std::move(token), std::string(buf.data(), buf.size()));
		}

#ifdef CINATRA_ENABLE_SSL
        void set_ssl_context_callback(std::function<void(boost::asio::ssl::context&)> ssl_context_callback) {
            ssl_context_callback_ = std::move(ssl_context_callback);
        }
#endif

	private:
		void async_connect() {
			assert(port_ != 0);
			auto addr = boost::asio::ip::address::from_string(host_);
			socket_.async_connect({ addr, port_ }, [this](const boost::system::error_code& ec) {
				if (has_connected_) {
					return;
				}

				if (ec) {
					//std::cout << ec.message() << std::endl;

					has_connected_ = false;

					if (reconnect_cnt_ == 0) {
						return;
					}

					if (reconnect_cnt_ > 0) {
						reconnect_cnt_--;
					}

					async_reconnect();
				}
				else {
					//std::cout<<"connected ok"<<std::endl;					
                    if (is_ssl()) {
                        handshake();
                        return;
                    }

                    has_connected_ = true;
					do_read();
					resend_subscribe();
					if (has_wait_)
						conn_cond_.notify_one();
				}
			});
		}

		void async_reconnect() {
			reset_socket();
			async_connect();
			std::this_thread::sleep_for(std::chrono::milliseconds(connect_timeout_));
		}

		void reset_deadline_timer(size_t timeout) {
			deadline_.expires_from_now(std::chrono::seconds(timeout));
			deadline_.async_wait([this, timeout](const boost::system::error_code& ec) {
				if (!ec) {
					if (has_connected_) {
						write(0, request_type::req_res, buffer_type(0));
					}
				}

				reset_deadline_timer(timeout);
			});
		}

		void write(std::uint64_t req_id, request_type type, buffer_type&& message) {
			size_t size = message.size();
			assert(size < MAX_BUF_LEN);
			client_message_type msg{ req_id, type, {message.release(), size} };

			std::unique_lock<std::mutex> lock(write_mtx_);
			outbox_.emplace_back(std::move(msg));
			if (outbox_.size() > 1) {
				// outstanding async_write
				return;
			}

			write();
		}

		void write() {
			auto& msg = outbox_[0];
			write_size_ = (uint32_t)msg.content.length();
            std::array<boost::asio::const_buffer, 4> write_buffers;
            write_buffers[0] = boost::asio::buffer(&write_size_, sizeof(int32_t));
            write_buffers[1] = boost::asio::buffer(&msg.req_id, sizeof(uint64_t));
            write_buffers[2] = boost::asio::buffer(&msg.req_type, sizeof(request_type));
            write_buffers[3] = boost::asio::buffer((char*)msg.content.data(), write_size_);

			async_write(write_buffers,
				[this](const boost::system::error_code& ec, const size_t length) {
				if (ec) {
					has_connected_ = false;
					close(false);
					error_callback(ec);

					return;
				}

				std::unique_lock<std::mutex> lock(write_mtx_);
				if (outbox_.empty()) {
					return;
				}

				::free((char*)outbox_.front().content.data());
				outbox_.pop_front();

				if (!outbox_.empty()) {
					// more messages to send
					this->write();
				}
			});
		}

		void do_read() {
            async_read_head([this](const boost::system::error_code& ec, const size_t length) {
				if (!socket_.is_open()) {
					//LOG(INFO) << "socket already closed";
					has_connected_ = false;
					return;
				}

				if (!ec) {
					//const uint32_t body_len = *((uint32_t*)(head_));
					//auto req_id = *((std::uint64_t*)(head_ + sizeof(int32_t)));
					//auto req_type = *(request_type*)(head_ + sizeof(int32_t) + sizeof(int64_t));
					rpc_header* header = (rpc_header*)(head_);
					const uint32_t body_len = header->body_len;
					if (body_len > 0 && body_len < MAX_BUF_LEN) {
						if (body_.size() < body_len) { body_.resize(body_len); }
						read_body(header->req_id, header->req_type, body_len);
						return;
					}

					if (body_len == 0 || body_len > MAX_BUF_LEN) {
						//LOG(INFO) << "invalid body len";
                        close();
						error_callback(asio::error::make_error_code(asio::error::message_size));
						return;
					}
				}
				else {
					close(false);
					error_callback(ec);
				}
			});
		}

		void read_body(std::uint64_t req_id, request_type req_type, size_t body_len) {
            async_read(body_len,
				[this, req_id, req_type, body_len](boost::system::error_code ec, std::size_t length) {
				//cancel_timer();

				if (!socket_.is_open()) {
					//LOG(INFO) << "socket already closed";
					call_back(req_id, asio::error::make_error_code(asio::error::connection_aborted), {});
					return;
				}

				if (!ec) {
					//entier body
					if (req_type == request_type::req_res) {
						call_back(req_id, ec, { body_.data(), body_len });
					}
					else if (req_type == request_type::sub_pub) {
						callback_sub(ec, { body_.data(), body_len });
					}
					else {
						close();
						error_callback(asio::error::make_error_code(asio::error::invalid_argument));
						return;
					}

					do_read();
				}
				else {
					//LOG(INFO) << ec.message();
					has_connected_ = false;
                    close();
					error_callback(ec);
				}
			});
		}

		void send_subscribe(const std::string& key, const std::string& token) {
			msgpack_codec codec;
			auto ret = codec.pack_args(key, token);
			write(0, request_type::sub_pub, std::move(ret));
		}

		void resend_subscribe() {
			if (key_token_set_.empty())
				return;

			for (auto& pair : key_token_set_) {
				send_subscribe(pair.first, pair.second);
			}
		}

		void call_back(uint64_t req_id, const boost::system::error_code& ec, string_view data) {
			temp_req_id_ = req_id;
			auto cb_flag = req_id >> 63;
			if (cb_flag) {
				std::shared_ptr<call_t> cl = nullptr;
				{
					std::unique_lock<std::mutex> lock(cb_mtx_);
					cl = std::move(callback_map_[req_id]);
				}

				assert(cl);
				if (!cl->has_timeout()) {
					cl->cancel();
					cl->callback(ec, data);
				}
				else {
					cl->callback(asio::error::make_error_code(asio::error::timed_out), {});
				}

				std::unique_lock<std::mutex> lock(cb_mtx_);
				callback_map_.erase(req_id);
			}
			else {
				std::unique_lock<std::mutex> lock(cb_mtx_);
				auto& f = future_map_[req_id];
				if (ec) {
					//LOG<<ec.message();
					if (!f) {
						//std::cout << "invalid req_id" << std::endl;
						return;
					}
				}

				assert(f);
				f->set_value(req_result{ data });
				future_map_.erase(req_id);
			}
		}

		void callback_sub(const boost::system::error_code& ec, string_view result) {
			rpc_service::msgpack_codec codec;
			try {
				auto tp = codec.unpack<std::tuple<int, std::string, std::string>>(result.data(), result.size());
				auto code = std::get<0>(tp);
				auto& key = std::get<1>(tp);
				auto& data = std::get<2>(tp);

				auto it = sub_map_.find(key);
				if (it == sub_map_.end()) {
					return;
				}

				it->second(data);
			}
			catch (const std::exception& /*ex*/) {
				error_callback(asio::error::make_error_code(asio::error::invalid_argument));
			}			
		}

		void clear_cache() {
			{
				std::unique_lock<std::mutex> lock(write_mtx_);
				while (!outbox_.empty()) {
					::free((char*)outbox_.front().content.data());
					outbox_.pop_front();
				}
			}

			{
				std::unique_lock<std::mutex> lock(cb_mtx_);
				callback_map_.clear();
				future_map_.clear();
			}
		}

        void reset_socket(){
            boost::system::error_code igored_ec;
            socket_.close(igored_ec);
            socket_ = decltype(socket_)(ios_);
            if(!socket_.is_open()){
                socket_.open(boost::asio::ip::tcp::v4());
            }
        }

		class call_t : asio::noncopyable, public std::enable_shared_from_this<call_t> {
		public:
			call_t(asio::io_service& ios, std::function<void(boost::system::error_code, string_view)> cb, size_t timeout) : timer_(ios),
				cb_(std::move(cb)), timeout_(timeout){
			}

			void start_timer() {
				if (timeout_ == 0) {
					return;
				}

				timer_.expires_from_now(std::chrono::milliseconds(timeout_));
				auto self = this->shared_from_this();
				timer_.async_wait([this, self](boost::system::error_code ec) {
					if (ec) {
						return;
					}

					has_timeout_ = true;
				});
			}

			void callback(boost::system::error_code ec, string_view data) {
				cb_(ec, data);
			}

			bool has_timeout() const {
				return has_timeout_;
			}

			void cancel() {
				if (timeout_ == 0) {
					return;
				}

				boost::system::error_code ec;
				timer_.cancel(ec);
			}

		private:
			boost::asio::steady_timer timer_;
			std::function<void(boost::system::error_code, string_view)> cb_;
			size_t timeout_;
			bool has_timeout_ = false;
		};

		void error_callback(const boost::system::error_code& ec) {
			if (err_cb_) {
				err_cb_(ec);
			}

			if (enable_reconnect_) {
				async_connect();
			}
		}

		void set_default_error_cb() {
			err_cb_ = [this](boost::system::error_code){
				async_connect();
			};
		}

        bool is_ssl() const {
#ifdef CINATRA_ENABLE_SSL
            return ssl_stream_ != nullptr;
#else
            return false;
#endif
        }


        void handshake() {
#ifdef CINATRA_ENABLE_SSL
            ssl_stream_->async_handshake(boost::asio::ssl::stream_base::client,
                [this](const boost::system::error_code& ec) {
                if (!ec) {
                    has_connected_ = true;
                    do_read();
                    resend_subscribe();
                    if (has_wait_)
                        conn_cond_.notify_one();
                }
                else {
                    error_callback(ec);
                    close();
                }
            });
#endif
        }

        void upgrade_to_ssl() {
#ifdef CINATRA_ENABLE_SSL
            if (ssl_stream_)
                return;

            boost::asio::ssl::context ssl_context(boost::asio::ssl::context::sslv23);
            ssl_context.set_default_verify_paths();
            boost::system::error_code ec;
            ssl_context.set_options(boost::asio::ssl::context::default_workarounds, ec);
            if (ssl_context_callback_) {
                ssl_context_callback_(ssl_context);
            }
            ssl_stream_ = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>>(socket_, ssl_context);
            //verify peer TODO
#else
            assert(is_ssl());//please add definition CINATRA_ENABLE_SSL, not allowed coming in this branch
#endif
        }

        template<typename Handler>
        void async_read_head(Handler handler) {
            if (is_ssl()) {
#ifdef CINATRA_ENABLE_SSL
                boost::asio::async_read(*ssl_stream_, boost::asio::buffer(head_, HEAD_LEN), std::move(handler));
#endif
            }
            else {
                boost::asio::async_read(socket_, boost::asio::buffer(head_, HEAD_LEN), std::move(handler));
            }
        }

        template<typename Handler>
        void async_read(size_t size_to_read, Handler handler) {
            if (is_ssl()) {
#ifdef CINATRA_ENABLE_SSL
                boost::asio::async_read(*ssl_stream_, boost::asio::buffer(body_.data(), size_to_read), std::move(handler));
#endif
            }
            else {
                boost::asio::async_read(socket_, boost::asio::buffer(body_.data(), size_to_read), std::move(handler));
            }
        }

        template<typename BufferType, typename Handler>
        void async_write(const BufferType& buffers, Handler handler) {
            if (is_ssl()) {
#ifdef CINATRA_ENABLE_SSL
                boost::asio::async_write(*ssl_stream_, buffers, std::move(handler));
#endif
            }
            else {
                boost::asio::async_write(socket_, buffers, std::move(handler));
            }
        }

		boost::asio::io_service ios_;
		asio::ip::tcp::socket socket_;
#ifdef CINATRA_ENABLE_SSL
        std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>> ssl_stream_;
        std::function<void(boost::asio::ssl::context&)> ssl_context_callback_;
#endif
		boost::asio::io_service::work work_;
		std::shared_ptr<std::thread> thd_ = nullptr;

		std::string host_;
		unsigned short port_ = 0;
		size_t connect_timeout_ = 1000;//s
		int reconnect_cnt_ = -1;
		std::atomic_bool has_connected_ = { false };
		std::mutex conn_mtx_;
		std::condition_variable conn_cond_;
		bool has_wait_ = false;

		asio::steady_timer deadline_;

		struct client_message_type {
			std::uint64_t req_id;
			request_type req_type;
			string_view content;
		};
		std::deque<client_message_type> outbox_;
		uint32_t write_size_ = 0;
		std::mutex write_mtx_;
		uint64_t fu_id_ = 0;
		std::function<void(boost::system::error_code)> err_cb_;
		bool enable_reconnect_ = false;

		std::unordered_map<std::uint64_t, std::shared_ptr<std::promise<req_result>>> future_map_;
		std::unordered_map<std::uint64_t, std::shared_ptr<call_t>> callback_map_;
		std::mutex cb_mtx_;
		uint64_t callback_id_ = 0;

		uint64_t temp_req_id_ = 0;

		char head_[HEAD_LEN] = {};
		std::vector<char> body_;

		std::unordered_map<std::string, std::function<void(string_view)>> sub_map_;
		std::set<std::pair<std::string, std::string>> key_token_set_;
	};
}