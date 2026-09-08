#pragma once

#include "asio_util.hpp"
#include "codec.h"
#include "error_code.h"
#include "traits.h"
#include "use_asio.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <tuple>

namespace rest_rpc {

class rpc_context {
public:
  using response_handler =
      std::function<asio::awaitable<std::error_code>(std::string)>;

  rpc_context();

  rpc_context(asio::any_io_executor executor, response_handler responder)
      : executor_(executor), responder_(std::move(responder)) {}

  auto get_executor() const { return executor_; }

  bool has_response() const {
    return has_response_->load(std::memory_order_acquire);
  }

  template <auto func, typename... Args>
  asio::awaitable<std::error_code> response_s(Args &&...args) {
    using result_type = return_type_t<
        typename util::function_traits<decltype(func)>::return_type>;
    static_assert(
        std::is_constructible_v<result_type, Args...>,
        "rpc function return type and response arguments are not match");
    return response(std::forward<Args>(args)...);
  }

  template <typename... Args>
  asio::awaitable<std::error_code> response(Args &&...args) {
    if (!responder_) {
      co_return make_error_code(rpc_errc::rpc_context_init_failed);
    }

    auto owned_args = std::tuple{own_response_arg(std::forward<Args>(args))...};
    auto buffer = std::apply(
        [](auto &...values) {
          return rpc_codec::pack_args(std::move(values)...);
        },
        owned_args);
    std::string body;
    if (!buffer.empty()) {
      body.assign(buffer.data(), buffer.size());
    }

    bool expected = false;
    if (!has_response_->compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
      co_return make_error_code(rpc_errc::has_response);
    }
    co_return co_await responder_(std::move(body));
  }

private:
  template <typename T> static auto own_response_arg(T &&value) {
    if constexpr (util::CharArray<T> ||
                  std::is_same_v<std::remove_cvref_t<T>, std::string_view>) {
      return std::string(std::string_view(value));
    } else {
      return std::decay_t<T>(std::forward<T>(value));
    }
  }

  asio::any_io_executor executor_;
  response_handler responder_;
  std::shared_ptr<std::atomic_bool> has_response_ =
      std::make_shared<std::atomic_bool>(false);
};

} // namespace rest_rpc
