#pragma once
#include <atomic>
#include <thread>
#include <chrono>

class qps {
public:
	void increase() {
		counter_.fetch_add(1, std::memory_order_release);
	}

	qps() {
		thd_ = std::thread([this] {
			while (!stop_) {
				std::cout << "qps: " << counter_.load(std::memory_order_acquire) << '\n';
				std::this_thread::sleep_for(std::chrono::seconds(1));
				//counter_.store(0, std::memory_order_release);
			}
		});
	}

	~qps() {
		stop_ = true;
		thd_.join();
	}

private:
	bool stop_ = false;
	std::thread thd_;
	std::atomic<uint32_t> counter_=0;
};