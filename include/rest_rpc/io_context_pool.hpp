#pragma once
#include <asio.hpp>
#include <vector>

namespace rest_rpc {
class io_context_pool {
public:
  explicit io_context_pool(size_t pool_size) {
    if (pool_size == 0) {
      pool_size = 1;
    }

    for (size_t i = 0; i < pool_size; i++) {
      auto io_ctx = std::make_shared<asio::io_context>();
      works_.push_back(asio::make_work_guard(*io_ctx));
      io_contexts_.emplace_back(io_ctx);
    }
  }

  ~io_context_pool() { stop(); }

  void run() {
    std::call_once(run_flag_, [this] {
      std::vector<std::thread> threads;
      for (auto &ctx : io_contexts_) {
        threads.push_back(std::thread([&ctx] { ctx->run(); }));
      }

      for (auto &thd : threads) {
        thd.join();
      }
    });
  }

  void stop() {
    std::call_once(stop_flag_, [this] { works_.clear(); });
  }

  size_t size() const { return io_contexts_.size(); }

  std::shared_ptr<asio::io_context> &get_io_context_ptr() {
    size_t i = next_.fetch_add(1, std::memory_order::relaxed);
    return io_contexts_[i % io_contexts_.size()];
  }

  asio::io_context &get_io_context() { return *get_io_context_ptr(); }

  auto get_executor() {
    auto &ctx = get_io_context();
    return ctx.get_executor();
  }

private:
  std::vector<std::shared_ptr<asio::io_context>> io_contexts_;
  std::vector<asio::executor_work_guard<asio::io_context::executor_type>>
      works_;
  std::once_flag run_flag_;
  std::once_flag stop_flag_;
  std::atomic<size_t> next_ = 0;
};

inline auto
get_global_executor(unsigned pool_size = std::thread::hardware_concurrency()) {
  static auto g_io_context_pool = std::make_shared<io_context_pool>(pool_size);
  [[maybe_unused]] static bool run_helper = [](auto pool) {
    std::thread thrd{[pool] { pool->run(); }};
    thrd.detach();
    return true;
  }(g_io_context_pool);
  return g_io_context_pool->get_executor();
}
} // namespace rest_rpc