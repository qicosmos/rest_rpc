#pragma once
#include "traits.h"
#include "use_asio.hpp"

namespace rest_rpc {
template <typename T>
constexpr inline bool is_awaitable_v =
    util::is_specialization_v<std::remove_cvref_t<T>, asio::awaitable>;

template <typename T, bool IsAwaitable = is_awaitable_v<T>> struct return_type {
  using type = T;
};

template <typename T> struct return_type<T, true> {
  using type = typename std::remove_cvref_t<T>::value_type;
};

template <typename T> using return_type_t = typename return_type<T>::type;

template <typename Coro> inline auto async_start(auto executor, Coro &&coro) {
  asio::co_spawn(executor, std::forward<Coro>(coro), asio::detached);
}

template <typename Coro> inline auto async_future(auto executor, Coro &&coro) {
  return asio::co_spawn(executor, std::forward<Coro>(coro), asio::use_future);
}

template <typename Coro> inline auto sync_wait(auto executor, Coro &&coro) {
  return async_future(executor, std::forward<Coro>(coro)).get();
}

inline auto async_start(auto executor, auto &&coro, auto callback) {
  using R = typename std::remove_cvref_t<decltype(coro)>::value_type;
  if constexpr (std::is_void_v<R>) {
    asio::co_spawn(
        executor, std::move(coro),
        asio::any_completion_handler<void(std::exception_ptr)>(
            [cb = std::move(callback)](std::exception_ptr e) { cb(e); }));
  } else {
    asio::co_spawn(executor, std::move(coro),
                   asio::any_completion_handler<void(std::exception_ptr, R)>(
                       [cb = std::move(callback)](std::exception_ptr e, R r) {
                         cb(e, std::move(r));
                       }));
  }
}
} // namespace rest_rpc