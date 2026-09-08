#pragma once

#include "rpc_context.hpp"

namespace rest_rpc {

struct multiplex_rpc_result {
  rpc_errc ec = rpc_errc::ok;
  std::string body;
  bool suppress_response = false;
};

template <typename Function>
constexpr bool rpc_router::is_multiplex_context_handler() {
  using args_tuple = typename util::function_traits<Function>::parameters_type;
  if constexpr (std::tuple_size_v<args_tuple> == 0) {
    return false;
  } else {
    using first_t = std::remove_cv_t<
        std::remove_reference_t<std::tuple_element_t<0, args_tuple>>>;
    return std::is_same_v<first_t, rpc_context>;
  }
}

inline asio::awaitable<multiplex_rpc_result>
rpc_router::route_multiplex(uint32_t key, std::string_view data,
                            rpc_context &context) {
  auto it = multiplex_invokers_.find(key);
  if (it == multiplex_invokers_.end()) {
    auto response = co_await route(key, data);
    co_return multiplex_rpc_result{response.ec, std::string(response.data()),
                                   false};
  }

  rpc_result response{};
  bool suppress_response = false;
  try {
    co_await it->second(data, response, context);
    suppress_response = true;
  } catch (const std::exception &ex) {
    suppress_response = context.has_response();
    response.result =
        std::string("exception occur when call").append(ex.what());
    response.ec = rpc_errc::function_exception;
  } catch (...) {
    suppress_response = context.has_response();
    response.result = std::string("unknown exception occur when call ")
                          .append(get_name_by_key(key));
    response.ec = rpc_errc::function_unknown_exception;
  }

  // A multiplexed response outlives this routing coroutine. Always make the
  // payload owning before returning so a string_view into coroutine-local
  // storage cannot escape and be copied by the server after frame teardown.
  co_return multiplex_rpc_result{response.ec, std::string(response.data()),
                                 suppress_response};
}

template <typename Function, typename Self>
void rpc_router::register_multiplex_func_impl(uint32_t key, const Function &f,
                                              Self *self) {
  multiplex_invokers_[key] =
      [this, f, self](std::string_view str, rpc_result &ret,
                      rpc_context &context) -> asio::awaitable<void> {
    using args_tuple =
        typename util::function_traits<Function>::parameters_type;
    using args = util::remove_first_t<args_tuple>;
    using R = typename util::function_traits<Function>::return_type;

    co_await handle_multiplex_context<R, args>(str, f, ret, self, context);
  };
}

template <typename R, typename Args, typename F, typename Self>
asio::awaitable<void>
rpc_router::handle_multiplex_context(std::string_view str, const F &f,
                                     rpc_result &ret, Self *self,
                                     rpc_context &context) {
  auto rpc_args = [&] {
    if constexpr (std::tuple_size_v<Args> == 0) {
      return Args{};
    } else if constexpr (std::tuple_size_v<Args> == 1 &&
                         util::is_basic_v<std::tuple_element_t<0, Args>>) {
      using arg_type = std::tuple_element_t<0, Args>;
      return Args{rpc_codec::unpack<arg_type>(str)};
    } else {
      return rpc_codec::unpack<Args>(str);
    }
  }();

  auto args =
      std::tuple_cat(std::tuple<rpc_context>{context}, std::move(rpc_args));
  auto invoke = [self, &f](auto &&...values) -> decltype(auto) {
    if constexpr (std::is_void_v<Self>) {
      return f(std::forward<decltype(values)>(values)...);
    } else {
      return ((*self).*f)(std::forward<decltype(values)>(values)...);
    }
  };

  if constexpr (is_void_v<R>) {
    if constexpr (is_awaitable_v<R>) {
      co_await std::apply(invoke, args);
    } else {
      std::apply(invoke, args);
    }
  } else {
    if constexpr (is_awaitable_v<R>) {
      ret = rpc_codec::pack_args(co_await std::apply(invoke, args));
    } else {
      ret = rpc_codec::pack_args(std::apply(invoke, args));
    }
  }
  co_return;
}

} // namespace rest_rpc
