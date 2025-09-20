#pragma once
#include "codec.h"
#include "error_code.h"
#include "function_name.h"
#include "md5.hpp"
#include "meta_util.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace rest_rpc {
struct rpc_result {
  rpc_errc ec = rpc_errc::ok;
  std::string result;
};

template <auto func> constexpr uint32_t get_key() {
  constexpr auto name = get_func_name<func>();
  constexpr uint32_t key = MD5::MD5Hash32(name.data(), name.length());
  return key;
}

class rpc_router {
public:
  template <typename Function, typename Self = void>
  void register_handler(std::string_view name, const Function &f,
                        Self *self = nullptr) {
    uint32_t key = MD5::MD5Hash32(name.data(), name.length());
    register_handler_impl(key, name, f, self);
  }

  template <auto func, typename Self = void>
  void register_handler(Self *self = nullptr) {
    constexpr auto name = get_func_name<func>();
    return register_handler(name, func, self);
  }

  void remove_handler(std::string_view name) {
    uint32_t key = MD5::MD5Hash32(name.data(), name.length());
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

  rpc_result route(uint32_t key, std::string_view data) {
    rpc_result route_result{};
    std::string result;
    try {
      rpc_service::msgpack_codec codec;
      auto it = map_invokers_.find(key);
      if (it == map_invokers_.end()) {
        result = "unknown function: " + get_name_by_key(key);
        route_result.ec = rpc_errc::no_such_function;
      } else {
        it->second(data, route_result.ec, result);
        route_result.ec = rpc_errc::ok;
      }
    } catch (const std::exception &ex) {
      rpc_service::msgpack_codec codec;
      result = std::string("exception occur when call").append(ex.what());
      route_result.ec = rpc_errc::function_exception;
    } catch (...) {
      rpc_service::msgpack_codec codec;
      result = std::string("unknown exception occur when call ")
                   .append(get_name_by_key(key));
      route_result.ec = rpc_errc::function_unknown_exception;
    }

    route_result.result = std::move(result);

    return route_result;
  }

private:
  template <typename Function, typename Self = void>
  auto register_handler_impl(uint32_t key, std::string_view name,
                             const Function &f, Self *self = nullptr) {
    if (key2func_name_.find(key) != key2func_name_.end()) {
      throw std::invalid_argument("duplicate registration key !");
    } else {
      key2func_name_.emplace(key, name);
      if constexpr (std::is_void_v<Self>) {
        return register_nonmember_func(key, f);
      } else {
        return register_member_func(key, f, self);
      }
    }
  }

  template <typename Function>
  void register_nonmember_func(uint32_t key, Function f) {
    this->map_invokers_[key] = [f = std::move(f)](std::string_view str,
                                                  rpc_errc &ec,
                                                  std::string &result) mutable {
      using args_tuple = typename function_traits<Function>::tuple_type;
      using R = typename function_traits<Function>::return_type;
      rpc_service::msgpack_codec codec;
      try {
        auto tp = codec.unpack<args_tuple>(str.data(), str.size());
        if constexpr (std::is_void_v<R>) {
          std::apply(f, tp);
        } else {
          auto r = std::apply(f, tp);
          result = rpc_service::msgpack_codec::pack_to_string(r);
        }
      } catch (std::invalid_argument &e) {
        ec = rpc_errc::invalid_argument;
        result = e.what();
      } catch (const std::exception &e) {
        ec = rpc_errc::function_exception;
        result = e.what();
      }
    };
  }

  template <typename Function, typename Self>
  void register_member_func(uint32_t key, const Function &f, Self *self) {
    this->map_invokers_[key] = [f, self](std::string_view str, rpc_errc &ec,
                                         std::string &result) {
      using args_tuple = typename function_traits<Function>::tuple_type;
      using R = typename function_traits<Function>::return_type;
      rpc_service::msgpack_codec codec;
      try {
        auto tp = codec.unpack<args_tuple>(str.data(), str.size());

        if constexpr (std::is_void_v<R>) {
          std::apply(
              [self, &f](auto &&...args) {
                return (*self.*f)(std::forward<decltype(args)>(args)...);
              },
              tp);
        } else {
          auto r = std::apply(
              [self, &f](auto &&...args) {
                return (*self.*f)(std::forward<decltype(args)>(args)...);
              },
              tp);
          result = rpc_service::msgpack_codec::pack_to_string(r);
        }
      } catch (std::invalid_argument &e) {
        ec = rpc_errc::invalid_argument;
        result = e.what();
      } catch (const std::exception &e) {
        ec = rpc_errc::function_exception;
        result = e.what();
      }
    };
  }

  std::unordered_map<uint32_t, std::function<void(std::string_view, rpc_errc &,
                                                  std::string &)>>
      map_invokers_;
  std::unordered_map<uint32_t, std::string> key2func_name_;
};
} // namespace rest_rpc