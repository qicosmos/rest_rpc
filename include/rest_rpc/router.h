#ifndef REST_RPC_ROUTER_H_
#define REST_RPC_ROUTER_H_

#include "codec.h"
#include "md5.hpp"
#include "meta_util.hpp"
#include "use_asio.hpp"
#include <functional>
#include <string>
#include <unordered_map>

namespace rest_rpc {
enum class ExecMode { sync, async };
const constexpr ExecMode Async = ExecMode::async;

namespace rpc_service {
class connection;

class router : asio::noncopyable {
public:
  template <ExecMode model, typename Function>
  void register_handler(std::string const &name, Function f) {
    uint32_t key = MD5::MD5Hash32(name.data());
    key2func_name_.emplace(key, name);
    return register_nonmember_func<model>(key, std::move(f));
  }

  template <ExecMode model, typename Function, typename Self>
  void register_handler(std::string const &name, const Function &f,
                        Self *self) {
    uint32_t key = MD5::MD5Hash32(name.data());
    key2func_name_.emplace(key, name);
    return register_member_func<model>(key, f, self);
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
  void route(uint32_t key, const char *data, std::size_t size,
             std::weak_ptr<T> conn) {
    auto conn_sp = conn.lock();
    if (!conn_sp) {
      return;
    }

    auto req_id = conn_sp->request_id();
    std::string result;
    try {
      msgpack_codec codec;
      auto it = map_invokers_.find(key);
      if (it == map_invokers_.end()) {
        result = codec.pack_args_str(
            result_code::FAIL, "unknown function: " + get_name_by_key(key));
        conn_sp->response(req_id, std::move(result));
        return;
      }

      ExecMode model;
      it->second(conn, data, size, result, model);
      if (model == ExecMode::sync) {
        if (result.size() >= MAX_BUF_LEN) {
          result = codec.pack_args_str(
              result_code::FAIL,
              "the response result is out of range: more than 10M " +
                  get_name_by_key(key));
        }
        conn_sp->response(req_id, std::move(result));
      }
    } catch (const std::exception &ex) {
      msgpack_codec codec;
      result = codec.pack_args_str(result_code::FAIL, ex.what());
      conn_sp->response(req_id, std::move(result));
    }
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

  template <typename Function, ExecMode mode = ExecMode::sync> struct invoker {
    template <ExecMode model>
    static inline void apply(const Function &func,
                             std::weak_ptr<connection> conn, const char *data,
                             size_t size, std::string &result,
                             ExecMode &exe_model) {
      using args_tuple = typename function_traits<Function>::bare_tuple_type;
      exe_model = ExecMode::sync;
      msgpack_codec codec;
      try {
        auto tp = codec.unpack<args_tuple>(data, size);
        call(func, conn, result, std::move(tp));
        exe_model = model;
      } catch (std::invalid_argument &e) {
        result = codec.pack_args_str(result_code::FAIL, e.what());
      } catch (const std::exception &e) {
        result = codec.pack_args_str(result_code::FAIL, e.what());
      }
    }

    template <ExecMode model, typename Self>
    static inline void apply_member(const Function &func, Self *self,
                                    std::weak_ptr<connection> conn,
                                    const char *data, size_t size,
                                    std::string &result, ExecMode &exe_model) {
      using args_tuple = typename function_traits<Function>::bare_tuple_type;
      exe_model = ExecMode::sync;
      msgpack_codec codec;
      try {
        auto tp = codec.unpack<args_tuple>(data, size);
        call_member(func, self, conn, result, std::move(tp));
        exe_model = model;
      } catch (std::invalid_argument &e) {
        result = codec.pack_args_str(result_code::FAIL, e.what());
      } catch (const std::exception &e) {
        result = codec.pack_args_str(result_code::FAIL, e.what());
      }
    }
  };

  /****
  注册  key名，函数名， 类名，参数类型，参数个数(变参模板)，connptr 连接指针
  ****/
  
  template <ExecMode model, typename Function>
  void register_nonmember_func(uint32_t key, Function f) {
    this->map_invokers_[key] = {std::bind(
        &invoker<Function>::template apply<model>, std::move(f),
        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
        std::placeholders::_4, std::placeholders::_5)};
  }

  template <ExecMode model, typename Function, typename Self>
  void register_member_func(uint32_t key, const Function &f, Self *self) {
    this->map_invokers_[key] = {std::bind(
        &invoker<Function>::template apply_member<model, Self>, f, self,
        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
        std::placeholders::_4, std::placeholders::_5)};
  }

  std::unordered_map<
      uint32_t, std::function<void(std::weak_ptr<connection>, const char *,
                                   size_t, std::string &, ExecMode &model)>>
      map_invokers_;
  std::unordered_map<uint32_t, std::string> key2func_name_;
};
} // namespace rpc_service
} // namespace rest_rpc

#endif // REST_RPC_ROUTER_H_
