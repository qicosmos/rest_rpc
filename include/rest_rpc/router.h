#ifndef REST_RPC_ROUTER_H_
#define REST_RPC_ROUTER_H_

#include "codec.h"
#include "md5.hpp"
#include "meta_util.hpp"
#include "string_view.hpp"
#include "use_asio.hpp"
#include <functional>
#include <string>
#include <unordered_map>

namespace rest_rpc {
namespace rpc_service {
class connection;

enum class router_error { ok, no_such_function, has_exception, unkonw };

struct route_result_t {
  router_error ec = router_error::unkonw;
  std::string result;
};

class router : asio::noncopyable {
public:
  template <typename Function>
  void register_handler(std::string const &name, Function f) {
    uint32_t key = MD5::MD5Hash32(name.data());
    key2func_name_.emplace(key, name);
    return register_nonmember_func(key, std::move(f));
  }

  template <typename Function, typename Self>
  void register_handler(std::string const &name, const Function &f,
                        Self *self) {
    uint32_t key = MD5::MD5Hash32(name.data());
    key2func_name_.emplace(key, name);
    return register_member_func(key, f, self);
  }

  void remove_handler(std::string const &name) {
    uint32_t key = MD5::MD5Hash32(name.data());
    this->map_invokers_.erase(key);
    key2func_name_.erase(key);
  }

  std::string get_name_by_key(uint32_t key) {
    auto it = key2func_name_.find(key);
    if (it != key2func_name_.end()) {
      return it->second;
    }
    return std::to_string(key);
  }

  template <typename T>
  route_result_t route(uint32_t key, nonstd::string_view data,
                       std::weak_ptr<T> conn) {
    route_result_t route_result{};
    std::string result;
    try {
      msgpack_codec codec;
      auto it = map_invokers_.find(key);
      if (it == map_invokers_.end()) {
        result = codec.pack_args_str(
            result_code::FAIL, "unknown function: " + get_name_by_key(key));
        route_result.ec = router_error::no_such_function;
      } else {
        it->second(conn, data, result);
        route_result.ec = router_error::ok;
      }
    } catch (const std::exception &ex) {
      msgpack_codec codec;
      result = codec.pack_args_str(
          result_code::FAIL,
          std::string("exception occur when call").append(ex.what()));
      route_result.ec = router_error::has_exception;
    } catch (...) {
      msgpack_codec codec;
      result = codec.pack_args_str(
          result_code::FAIL, std::string("unknown exception occur when call ")
                                 .append(get_name_by_key(key)));
      route_result.ec = router_error::no_such_function;
    }

    route_result.result = std::move(result);

    return route_result;
  }

  router() = default;

private:
  router(const router &) = delete;
  router(router &&) = delete;

  template <typename F, size_t... I, typename... Args>
  static typename std::result_of<F(std::weak_ptr<connection>, Args...)>::type
  call_helper(const F &f, const std::index_sequence<I...> &,
              std::tuple<Args...> tup, std::weak_ptr<connection> ptr) {
    return f(ptr, std::move(std::get<I>(tup))...);
  }

  template <typename F, typename... Args>
  static typename std::enable_if<std::is_void<typename std::result_of<
      F(std::weak_ptr<connection>, Args...)>::type>::value>::type
  call(const F &f, std::weak_ptr<connection> ptr, std::string &result,
       std::tuple<Args...> tp) {
    call_helper(f, std::make_index_sequence<sizeof...(Args)>{}, std::move(tp),
                ptr);
    result = msgpack_codec::pack_args_str(result_code::OK);
  }

  template <typename F, typename... Args>
  static typename std::enable_if<!std::is_void<typename std::result_of<
      F(std::weak_ptr<connection>, Args...)>::type>::value>::type
  call(const F &f, std::weak_ptr<connection> ptr, std::string &result,
       std::tuple<Args...> tp) {
    auto r = call_helper(f, std::make_index_sequence<sizeof...(Args)>{},
                         std::move(tp), ptr);
    msgpack_codec codec;
    result = msgpack_codec::pack_args_str(result_code::OK, r);
  }

  template <typename F, typename Self, size_t... Indexes, typename... Args>
  static
      typename std::result_of<F(Self, std::weak_ptr<connection>, Args...)>::type
      call_member_helper(const F &f, Self *self,
                         const std::index_sequence<Indexes...> &,
                         std::tuple<Args...> tup,
                         std::weak_ptr<connection> ptr =
                             std::shared_ptr<connection>{nullptr}) {
    return (*self.*f)(ptr, std::move(std::get<Indexes>(tup))...);
  }

  template <typename F, typename Self, typename... Args>
  static typename std::enable_if<std::is_void<typename std::result_of<
      F(Self, std::weak_ptr<connection>, Args...)>::type>::value>::type
  call_member(const F &f, Self *self, std::weak_ptr<connection> ptr,
              std::string &result, std::tuple<Args...> tp) {
    call_member_helper(f, self,
                       typename std::make_index_sequence<sizeof...(Args)>{},
                       std::move(tp), ptr);
    result = msgpack_codec::pack_args_str(result_code::OK);
  }

  template <typename F, typename Self, typename... Args>
  static typename std::enable_if<!std::is_void<typename std::result_of<
      F(Self, std::weak_ptr<connection>, Args...)>::type>::value>::type
  call_member(const F &f, Self *self, std::weak_ptr<connection> ptr,
              std::string &result, std::tuple<Args...> tp) {
    auto r = call_member_helper(
        f, self, typename std::make_index_sequence<sizeof...(Args)>{},
        std::move(tp), ptr);
    result = msgpack_codec::pack_args_str(result_code::OK, r);
  }

  template <typename Function>
  void register_nonmember_func(uint32_t key, Function f) {
    this->map_invokers_[key] = [f](std::weak_ptr<connection> conn,
                                   nonstd::string_view str,
                                   std::string &result) {
      using args_tuple = typename function_traits<Function>::bare_tuple_type;
      msgpack_codec codec;
      try {
        auto tp = codec.unpack<args_tuple>(str.data(), str.size());
        call(f, conn, result, std::move(tp));
      } catch (std::invalid_argument &e) {
        result = codec.pack_args_str(result_code::FAIL, e.what());
      } catch (const std::exception &e) {
        result = codec.pack_args_str(result_code::FAIL, e.what());
      }
    };
  }

  template <typename Function, typename Self>
  void register_member_func(uint32_t key, const Function &f, Self *self) {
    this->map_invokers_[key] = [f, self](std::weak_ptr<connection> conn,
                                         nonstd::string_view str,
                                         std::string &result) {
      using args_tuple = typename function_traits<Function>::bare_tuple_type;
      msgpack_codec codec;
      try {
        auto tp = codec.unpack<args_tuple>(str.data(), str.size());
        call_member(f, self, conn, result, std::move(tp));
      } catch (std::invalid_argument &e) {
        result = codec.pack_args_str(result_code::FAIL, e.what());
      } catch (const std::exception &e) {
        result = codec.pack_args_str(result_code::FAIL, e.what());
      }
    };
  }

  std::unordered_map<uint32_t,
                     std::function<void(std::weak_ptr<connection>,
                                        nonstd::string_view, std::string &)>>
      map_invokers_;
  std::unordered_map<uint32_t, std::string> key2func_name_;
};
} // namespace rpc_service
} // namespace rest_rpc

#endif // REST_RPC_ROUTER_H_
