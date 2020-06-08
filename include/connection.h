#ifndef REST_RPC_CONNECTION_H_
#define REST_RPC_CONNECTION_H_

#include <iostream>
#include <memory>
#include <array>
#include <deque>
#include "use_asio.hpp"
#include "const_vars.h"
#include "router.h"
#include "cplusplus_14.h"

using boost::asio::ip::tcp;

namespace rest_rpc {
	namespace rpc_service {
        struct ssl_configure {
            std::string cert_file;
            std::string key_file;
        };

		class connection : public std::enable_shared_from_this<connection>, private asio::noncopyable {
		public:
			connection(boost::asio::io_service& io_service, std::size_t timeout_seconds)
				: socket_(io_service),
				body_(INIT_BUF_SIZE),
				timer_(io_service),
				timeout_seconds_(timeout_seconds),
				has_closed_(false) {
			}

			~connection() { 
                close(); 
            }

			void start() { 
                if (is_ssl() && !has_shake_) {
                    async_handshake();
                }
                else {
                    read_head();
                }
            }

			tcp::socket& socket() { return socket_; }

			bool has_closed() const { return has_closed_; }
			uint64_t request_id() const {
				return req_id_;
			}

			void response(uint64_t req_id, std::string data, request_type req_type = request_type::req_res) {
				auto len = data.size();
				assert(len < MAX_BUF_LEN);

				std::unique_lock<std::mutex> lock(write_mtx_);
				write_queue_.emplace_back(message_type{ req_id, req_type, std::make_shared<std::string>(std::move(data)) });
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

			void set_conn_id(int64_t id) { conn_id_ = id; }

			int64_t conn_id() const { return conn_id_; }

			const std::vector<char>& body() const {
				return body_;
			}

			std::string remote_address() const {
				if (has_closed_) {
					return "";
				}

				return socket_.remote_endpoint().address().to_string();
			}
			
			void publish(const std::string& key, const std::string& data) {
				auto result = msgpack_codec::pack_args_str(result_code::OK, key, data);
				response(0, std::move(result), request_type::sub_pub);
			}

			void set_callback(std::function<void(std::string, std::string, std::weak_ptr<connection>)> callback) {
				callback_ = std::move(callback);
			}

            void init_ssl_context(const ssl_configure& ssl_conf) {
#ifdef CINATRA_ENABLE_SSL
                unsigned long ssl_options = boost::asio::ssl::context::default_workarounds
                    | boost::asio::ssl::context::no_sslv2
                    | boost::asio::ssl::context::single_dh_use;
                try {
                    boost::asio::ssl::context ssl_context(boost::asio::ssl::context::sslv23);
                    ssl_context.set_options(ssl_options);
                    ssl_context.set_password_callback([](std::size_t size,
                                                         boost::asio::ssl::context_base::password_purpose purpose) {return "123456"; });

                    boost::system::error_code ec;
                    if (fs::exists(ssl_conf.cert_file, ec)) {
                        ssl_context.use_certificate_chain_file(ssl_conf.cert_file);
                    }

                    if (fs::exists(ssl_conf.key_file, ec))
                        ssl_context.use_private_key_file(ssl_conf.key_file, boost::asio::ssl::context::pem);

                    //ssl_context_callback(ssl_context);
                    ssl_stream_ = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>>(socket_, ssl_context);
                }
                catch (const std::exception& e) {
                    print(e);
                }
#endif
            }

		private:
			void read_head() {
				reset_timer();
				auto self(this->shared_from_this());
                async_read_head([this, self](boost::system::error_code ec, std::size_t length) {
					if (!socket_.is_open()) {
						//LOG(INFO) << "socket already closed";
						return;
					}

					if (!ec) {
						//const uint32_t body_len = *((int*)(head_));
						//req_id_ = *((std::uint64_t*)(head_ + sizeof(int32_t)));
						rpc_header* header = (rpc_header*)(head_);
						req_id_ = header->req_id;
						const uint32_t body_len = header->body_len;
						req_type_ = header->req_type;
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
                            print("invalid body len");
							close();
						}
					}
					else {
                        print(ec);
						close();
					}
				});
			}

			void read_body(std::size_t size) {
				auto self(this->shared_from_this());
				async_read(size, [this, self](boost::system::error_code ec, std::size_t length) {
					cancel_timer();

					if (!socket_.is_open()) {
						//LOG(INFO) << "socket already closed";
						return;
					}

					if (!ec) {
						read_head();
						if (req_type_ == request_type::req_res) {
							router& _router = router::get();
							_router.route<connection>(body_.data(), length, this->shared_from_this());
						}
						else if (req_type_ == request_type::sub_pub) {
							try {
								msgpack_codec codec;
								auto p = codec.unpack<std::tuple<std::string, std::string>>(body_.data(), length);
								callback_(std::move(std::get<0>(p)), std::move(std::get<1>(p)), this->shared_from_this());
							}
							catch (const std::exception& ex) {
                                print(ex);
							}							
						}
					}
					else {
						//LOG(INFO) << ec.message();
					}
				});
			}

			void write() {
				auto& msg = write_queue_.front();
				write_size_ = (uint32_t)msg.content->size();
				std::array<boost::asio::const_buffer, 4> write_buffers;
				write_buffers[0] = boost::asio::buffer(&write_size_, sizeof(uint32_t));
				write_buffers[1] = boost::asio::buffer(&msg.req_id, sizeof(uint64_t));
				write_buffers[2] = boost::asio::buffer(&msg.req_type, sizeof(request_type));
				write_buffers[3] = boost::asio::buffer(msg.content->data(), write_size_);

				auto self = this->shared_from_this();
				async_write(write_buffers,
					[this, self](boost::system::error_code ec, std::size_t length) {
					on_write(ec, length);
				});
			}

			void on_write(boost::system::error_code ec, std::size_t length) {
				if (ec) {
                    print(ec);
					close(false);
					return;
				}

				if (has_closed()) { return; }

				std::unique_lock<std::mutex> lock(write_mtx_);
				write_queue_.pop_front();

				if (!write_queue_.empty()) {
					write();
				}
			}

            void async_handshake() {
#ifdef CINATRA_ENABLE_SSL
                auto self = this->shared_from_this();
                ssl_stream_->async_handshake(boost::asio::ssl::stream_base::server,
                    [this, self](const boost::system::error_code& error) {
                    if (error) {
                        print(error);
                        close();
                        return;
                    }

                    has_shake_ = true;
                    read_head();
                });
#endif
            }

            bool is_ssl() const {
#ifdef CINATRA_ENABLE_SSL
                return ssl_stream_ != nullptr;
#else
                return false;
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

			void reset_timer() {
				if (timeout_seconds_ == 0) { return; }

				auto self(this->shared_from_this());
				timer_.expires_from_now(std::chrono::seconds(timeout_seconds_));
				timer_.async_wait([this, self](const boost::system::error_code& ec) {
					if (has_closed()) { return; }

					if (ec) { return; }

					//LOG(INFO) << "rpc connection timeout";
					close(false);
				});
			}

			void cancel_timer() {
				if (timeout_seconds_ == 0) { return; }

				timer_.cancel();
			}

            void close(bool close_ssl = true) {
#ifdef CINATRA_ENABLE_SSL
                if (close_ssl && ssl_stream_) {
                    boost::system::error_code ec;
                    ssl_stream_->shutdown(ec);
                    ssl_stream_ = nullptr;
                }
#endif
                if (has_closed_) {
                    return;
                }

                boost::system::error_code ignored_ec;
                socket_.shutdown(tcp::socket::shutdown_both, ignored_ec);
                socket_.close(ignored_ec);
                has_closed_ = true;
                has_shake_ = false;
            }

            template<typename... Args>
            void print(Args... args) {
#ifdef _DEBUG
                std::initializer_list<int>{( std::cout << args << ' ', 0)...};
                std::cout << "\n";
#endif
            }

            void print(const boost::system::error_code& ec) {
                print(ec.value(), ec.message());
            }

            void print(const std::exception& ex) {
                print(ex.what());
            }

			tcp::socket socket_;
#ifdef CINATRA_ENABLE_SSL
            std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>> ssl_stream_ = nullptr;
#endif
            bool has_shake_ = false;
			char head_[HEAD_LEN];
			std::vector<char> body_;
			std::uint64_t req_id_;
			request_type req_type_;

			uint32_t write_size_ = 0;
			std::mutex write_mtx_;

			asio::steady_timer timer_;
			std::size_t timeout_seconds_;
			int64_t conn_id_ = 0;
			bool has_closed_;

			std::deque<message_type> write_queue_;
			std::function<void(std::string, std::string, std::weak_ptr<connection>)> callback_;
		};
	}  // namespace rpc_service
}  // namespace rest_rpc

#endif  // REST_RPC_CONNECTION_H_
