#pragma once
#include <atomic>
#include <chrono>
#include <thread>

class qps {
public:
  void increase() { counter_.fetch_add(1, std::memory_order_release); }

  qps() : counter_(0) {
    thd_ = std::thread([this] {
      while (!stop_) {
        auto val = counter_.load(std::memory_order_acquire);
        if(val==0) {
          continue;
        }
        
        std::cout << "qps: " << val
                  << '\n';
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // counter_.store(0, std::memory_order_release);
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
  std::atomic<uint32_t> counter_;
};