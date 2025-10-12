#pragma once
#include "codec.h"
#include "error_code.h"

#include "asio_util.hpp"
#include "util.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace rest_rpc {
template <typename T>
constexpr inline bool is_void_v =
    std::is_same_v<T, void> || std::is_same_v<T, asio::awaitable<void>>;

struct rpc_result {
  rpc_result(std::string str) : result(std::move(str)) {}
  rpc_result(std::string_view str) : view(str) {}
  rpc_result &operator=(std::string str) {
    result = std::move(str);
    return *this;
  }
  rpc_result &operator=(std::string_view str) {
    result = str;
    return *this;
  }
  rpc_result() = default;
  rpc_errc ec = rpc_errc::ok;
  std::string result;
  std::string_view view;
  bool empty() const { return result.empty() && view.empty(); }
  size_t size() const { return result.empty() ? view.size() : result.size(); }

  std::string_view data() const { return result.empty() ? view : result; }
};

class rpc_router {
public:
  template <typename Function, typename Self = void>
  void register_handler(std::string_view name, const Function &f,
                        Self *self = nullptr) {
    uint32_t key = MD5::MD5Hash32(name.data(), (uint32_t)name.length());
    register_handler_impl(key, name, f, self);
  }

  template <auto func, typename Self = void>
  void register_handler(Self *self = nullptr) {
    constexpr auto name = get_func_name<func>();
    return register_handler(name, func, self);
  }

  void remove_handler(std::string_view name) {
    uint32_t key = MD5::MD5Hash32(name.data(), (uint32_t)name.length());
    this->map_invokers_.erase(key);
    key2func_name_.erase(key);
  }

  template <auto func> void remove_handler() {
    constexpr std::string_view name = get_func_name<func>();
    remove_handler(name);
  }

  std::string get_name_by_key(uint32_t key) {
    auto it = key2func_name_.find(key);
    if (it != key2func_name_.end()) {
      return it->second;
    }
    return std::to_string(key);
  }

  asio::awaitable<rpc_result> route(uint32_t key, std::string_view data) {
    rpc_result route_result{};
    try {
      rpc_service::msgpack_codec codec;
      auto it = map_invokers_.find(key);
      if (it == map_invokers_.end()) {
        route_result.result = "unknown function: " + get_name_by_key(key);
        route_result.ec = rpc_errc::no_such_function;
      } else {
        co_await it->second(data, route_result);
        route_result.ec = rpc_errc::ok;
      }
    } catch (const std::exception &ex) {
      route_result.result =
          std::string("exception occur when call").append(ex.what());
      route_result.ec = rpc_errc::function_exception;
    } catch (...) {
      route_result.result = std::string("unknown exception occur when call ")
                                .append(get_name_by_key(key));
      route_result.ec = rpc_errc::function_unknown_exception;
    }

    co_return route_result;
  }

private:
  template <typename Function, typename Self = void>
  void register_handler_impl(uint32_t key, std::string_view name,
                             const Function &f, Self *self = nullptr) {
    if (key2func_name_.find(key) != key2func_name_.end()) {
      throw std::invalid_argument("duplicate registration key !");
    }

    key2func_name_.emplace(key, name);

    register_func_impl(key, f, self);
  }

  template <typename Function, typename Self>
  void register_func_impl(uint32_t key, const Function &f, Self *self) {
    this->map_invokers_[key] =
        [this, f, self](std::string_view str,
                        rpc_result &ret) -> asio::awaitable<void> {
      using args_tuple =
          typename util::function_traits<Function>::parameters_type;
      using R = typename util::function_traits<Function>::return_type;
      if constexpr (std::tuple_size_v<args_tuple> == 0) {
        co_await handle_zero_arg<R>(f, ret, self);
      } else {
        using first_t = std::tuple_element_t<0, args_tuple>;
        if constexpr (std::tuple_size_v<args_tuple> == 1 &&
                      util::is_basic_v<first_t>) {
          co_await handle_one_arg<R, first_t>(str, f, ret, self);
        } else {
          co_await handle_more_args<R, args_tuple>(str, f, ret, self);
        }
      }
    };
  }

  template <typename R, typename F, typename Self>
  asio::awaitable<void> handle_zero_arg(const F &f, rpc_result &ret,
                                        Self *self) {
    if constexpr (is_void_v<R>) {
      if constexpr (std::is_void_v<Self>) {
        if constexpr (is_awaitable_v<R>) {
          co_await f();
        } else {
          f();
        }
      } else {
        if constexpr (is_awaitable_v<R>) {
          co_await (*self.*f)();
        } else {
          (*self.*f)();
        }
      }
    } else {
      if constexpr (std::is_void_v<Self>) {
        if constexpr (is_awaitable_v<R>) {
          ret = rpc_service::msgpack_codec::pack_args(co_await f());
        } else {
          ret = rpc_service::msgpack_codec::pack_args(f());
        }
      } else {
        if constexpr (is_awaitable_v<R>) {
          ret = rpc_service::msgpack_codec::pack_args(co_await (*self.*f)());
        } else {
          ret = rpc_service::msgpack_codec::pack_args((*self.*f)());
        }
      }
    }
    co_return;
  }

  template <typename R, typename Arg, typename F, typename Self>
  asio::awaitable<void> handle_one_arg(std::string_view str, const F &f,
                                       rpc_result &ret, Self *self) {
    rpc_service::msgpack_codec codec;
    if constexpr (is_void_v<R>) {
      if constexpr (std::is_void_v<Self>) {
        if constexpr (is_awaitable_v<R>) {
          co_await f(codec.unpack<Arg>(str));
        } else {
          f(codec.unpack<Arg>(str));
        }
      } else {
        if constexpr (is_awaitable_v<R>) {
          co_await (*self.*f)(codec.unpack<Arg>(str));
        } else {
          (*self.*f)(codec.unpack<Arg>(str));
        }
      }
    } else {
      if constexpr (std::is_void_v<Self>) {
        if constexpr (is_awaitable_v<R>) {
          ret = rpc_service::msgpack_codec::pack_args(
              co_await f(codec.unpack<Arg>(str)));
        } else {
          ret =
              rpc_service::msgpack_codec::pack_args(f(codec.unpack<Arg>(str)));
        }
      } else {
        if constexpr (is_awaitable_v<R>) {
          ret = rpc_service::msgpack_codec::pack_args(
              co_await (*self.*f)(codec.unpack<Arg>(str)));
        } else {
          ret = rpc_service::msgpack_codec::pack_args(
              (*self.*f)(codec.unpack<Arg>(str)));
        }
      }
    }
    co_return;
  }

  template <typename R, typename Args, typename F, typename Self>
  asio::awaitable<void> handle_more_args(std::string_view str, const F &f,
                                         rpc_result &ret, Self *self) {
    rpc_service::msgpack_codec codec;
    auto tp = codec.unpack<Args>(str);
    if constexpr (std::is_void_v<R>) {
      if constexpr (std::is_void_v<Self>) {
        if constexpr (is_awaitable_v<R>) {
          co_await std::apply(f, tp);
        } else {
          std::apply(f, tp);
        }
      } else {
        if constexpr (is_awaitable_v<R>) {
          co_await std::apply(
              [self, &f](auto &&...args) {
                return (*self.*f)(std::forward<decltype(args)>(args)...);
              },
              tp);
        } else {
          std::apply(
              [self, &f](auto &&...args) {
                return (*self.*f)(std::forward<decltype(args)>(args)...);
              },
              tp);
        }
      }
    } else {
      if constexpr (std::is_void_v<Self>) {
        if constexpr (is_awaitable_v<R>) {
          ret =
              rpc_service::msgpack_codec::pack_args(co_await std::apply(f, tp));
        } else {
          ret = rpc_service::msgpack_codec::pack_args(std::apply(f, tp));
        }
      } else {
        if constexpr (is_awaitable_v<R>) {
          ret = rpc_service::msgpack_codec::pack_args(co_await std::apply(
              [self, &f](auto &&...args) {
                return (*self.*f)(std::forward<decltype(args)>(args)...);
              },
              tp));
        } else {
          ret = rpc_service::msgpack_codec::pack_args(std::apply(
              [self, &f](auto &&...args) {
                return (*self.*f)(std::forward<decltype(args)>(args)...);
              },
              tp));
        }
      }
    }
    co_return;
  }

  std::unordered_map<uint32_t, std::function<asio::awaitable<void>(
                                   std::string_view, rpc_result &)>>
      map_invokers_;
  std::unordered_map<uint32_t, std::string> key2func_name_;
};
} // namespace rest_rpc