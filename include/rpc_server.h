#ifndef REST_RPC_RPC_SERVER_H_
#define REST_RPC_RPC_SERVER_H_

#include <thread>
#include <mutex>
#include "connection.h"
#include "io_service_pool.h"
#include "router.h"

using boost::asio::ip::tcp;

namespace rest_rpc {
	namespace rpc_service {
		class retry_data : public std::enable_shared_from_this<retry_data> {
		public:
			retry_data(asio::io_service& ios, std::shared_ptr<std::string> data, size_t timeout) : timer_(ios), sp_data_(data),
				timeout_(timeout) {
			}

			void cancel() {
				if (timeout_ == 0) {
					return;
				}

				boost::system::error_code ec;
				timer_.cancel(ec);
			}

			bool has_timeout() const {
				return has_timeout_;
			}

			std::shared_ptr<std::string> data() {
				return sp_data_;
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
		private:
			boost::asio::steady_timer timer_;
			std::shared_ptr<std::string> sp_data_;
			size_t timeout_;
			std::atomic_bool has_timeout_ = { false };			
		};

		using rpc_conn = std::weak_ptr<connection>;
		class rpc_server : private asio::noncopyable {
		public:
			rpc_server(short port, size_t size, size_t timeout_seconds = 15, size_t check_seconds = 10)
				: io_service_pool_(size),
				acceptor_(io_service_pool_.get_io_service(), tcp::endpoint(tcp::v4(), port)),
				timeout_seconds_(timeout_seconds),
				check_seconds_(check_seconds) {
				do_accept();
				check_thread_ = std::make_shared<std::thread>([this] { clean(); });
				pub_sub_thread_ = std::make_shared<std::thread>([this] { clean_sub_pub(); });
			}

			~rpc_server() {
				stop_check_ = true;
				check_thread_->join();
				io_service_pool_.stop();
				if(thd_){
                    thd_->join();
				}
			}

			void async_run() {
				thd_ = std::make_shared<std::thread>([this] { io_service_pool_.run(); });
			}

			void run() {
                io_service_pool_.run();
			}

			template<ExecMode model = ExecMode::sync, typename Function>
			void register_handler(std::string const& name, const Function& f) {
				router::get().register_handler<model>(name, f);
			}

			template<ExecMode model = ExecMode::sync, typename Function, typename Self>
			void register_handler(std::string const& name, const Function& f, Self* self) {
				router::get().register_handler<model>(name, f, self);
			}

			void set_conn_timeout_callback(std::function<void(int64_t)> callback) {
				conn_timeout_callback_ = std::move(callback);
			}

			void publish(const std::string& key, std::string sub_key, std::string data) {
				decltype(sub_map_.equal_range(key)) range;

				{
					std::unique_lock<std::mutex> lock(sub_mtx_);
					if (sub_map_.empty())
						return;

					range = sub_map_.equal_range(key + sub_key);
				}
				
				auto shared_data = std::make_shared<std::string>(std::move(data));
				for (auto it = range.first; it != range.second; ++it) {
					auto conn = it->second.lock();
					if (conn == nullptr || conn->has_closed()) {
						if (!sub_key.empty()) {
							std::unique_lock<std::mutex> lock(retry_mtx_);
							auto it = retry_map_.find(sub_key);
							if (it == retry_map_.end())
								continue;

							auto retry = std::make_shared<retry_data>(io_service_pool_.get_io_service(),
								shared_data, 30 * 1000);
							retry_map_.emplace(std::move(sub_key), retry);
							retry->start_timer();
						}
						
						continue;
					}

					conn->publish(key + sub_key, *shared_data);
				}
			}

		private:
			void do_accept() {
				conn_.reset(new connection(io_service_pool_.get_io_service(), timeout_seconds_));
				conn_->set_callback([this](std::string key, std::string sub_key, std::weak_ptr<connection> conn) {
					std::unique_lock<std::mutex> lock(sub_mtx_);
					sub_map_.emplace(key + sub_key, conn);
				});

				acceptor_.async_accept(conn_->socket(), [this](boost::system::error_code ec) {
					if (ec) {
						//LOG(INFO) << "acceptor error: " << ec.message();
					}
					else {
						conn_->start();
						std::unique_lock<std::mutex> lock(mtx_);
						conn_->set_conn_id(conn_id_);
						connections_.emplace(conn_id_++, conn_);
					}

					do_accept();
				});
			}

			void clean() {
				while (!stop_check_) {
					std::this_thread::sleep_for(std::chrono::seconds(check_seconds_));

					std::unique_lock<std::mutex> lock(mtx_);
					for (auto it = connections_.cbegin(); it != connections_.cend();) {
						if (it->second->has_closed()) {
							if (conn_timeout_callback_) {
								conn_timeout_callback_(it->second->conn_id());
							}
							it = connections_.erase(it);
						}
						else {
							++it;
						}
					}
				}
			}

			void clean_sub_pub() {
				while (!stop_check_pub_sub_) {
					std::this_thread::sleep_for(std::chrono::seconds(10));

					std::unique_lock<std::mutex> lock(sub_mtx_);
					for (auto it = sub_map_.cbegin(); it != sub_map_.cend();) {
						auto conn = it->second.lock();
						if (conn == nullptr || conn->has_closed()) {
							it = sub_map_.erase(it);
						}
						else {
							++it;
						}
					}
				}
			}

			io_service_pool io_service_pool_;
			tcp::acceptor acceptor_;
			std::shared_ptr<connection> conn_;
			std::shared_ptr<std::thread> thd_;
			std::size_t timeout_seconds_;

			std::unordered_map<int64_t, std::shared_ptr<connection>> connections_;
			int64_t conn_id_ = 0;
			std::mutex mtx_;
			std::shared_ptr<std::thread> check_thread_;
			size_t check_seconds_;
			bool stop_check_ = false;

			std::function<void(int64_t)> conn_timeout_callback_;
			std::unordered_multimap<std::string, std::weak_ptr<connection>> sub_map_;
			std::mutex sub_mtx_;

			std::map<std::string, std::shared_ptr<retry_data>> retry_map_;
			std::mutex retry_mtx_;

			std::shared_ptr<std::thread> pub_sub_thread_;
			bool stop_check_pub_sub_ = false;
		};
	}  // namespace rpc_service
}  // namespace rest_rpc

#endif  // REST_RPC_RPC_SERVER_H_