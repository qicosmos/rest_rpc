#ifndef REST_RPC_META_UTIL_HPP
#define REST_RPC_META_UTIL_HPP

#include <functional>
#include "cplusplus_14.h"

namespace rest_rpc {

template <typename... Args, typename Func, std::size_t... Idx>
void for_each(const std::tuple<Args...> &t, Func &&f,
              std::index_sequence<Idx...>) {
  (void)std::initializer_list<int>{(f(std::get<Idx>(t)), void(), 0)...};
}

template <typename... Args, typename Func, std::size_t... Idx>
void for_each_i(const std::tuple<Args...> &t, Func &&f,
                std::index_sequence<Idx...>) {
  (void)std::initializer_list<int>{
      (f(std::get<Idx>(t), std::integral_constant<size_t, Idx>{}), void(),
       0)...};
}

template <typename T> struct function_traits;

template <typename Ret, typename Arg, typename... Args>
struct function_traits<Ret(Arg, Args...)> {
public:
  enum { arity = sizeof...(Args) + 1 };
  typedef Ret function_type(Arg, Args...);
  typedef Ret return_type;
  using stl_function_type = std::function<function_type>;
  typedef Ret (*pointer)(Arg, Args...);

  typedef std::tuple<Arg, Args...> tuple_type;
  typedef std::tuple<std::remove_const_t<std::remove_reference_t<Arg>>,
                     std::remove_const_t<std::remove_reference_t<Args>>...>
      bare_tuple_type;
  using args_tuple =
      std::tuple<std::string, Arg,
                 std::remove_const_t<std::remove_reference_t<Args>>...>;
  using args_tuple_2nd =
      std::tuple<std::string,
                 std::remove_const_t<std::remove_reference_t<Args>>...>;
};

template <typename Ret> struct function_traits<Ret()> {
public:
  enum { arity = 0 };
  typedef Ret function_type();
  typedef Ret return_type;
  using stl_function_type = std::function<function_type>;
  typedef Ret (*pointer)();

  typedef std::tuple<> tuple_type;
  typedef std::tuple<> bare_tuple_type;
  using args_tuple = std::tuple<std::string>;
  using args_tuple_2nd = std::tuple<std::string>;
};

template <typename Ret, typename... Args>
struct function_traits<Ret (*)(Args...)> : function_traits<Ret(Args...)> {};

template <typename Ret, typename... Args>
struct function_traits<std::function<Ret(Args...)>>
    : function_traits<Ret(Args...)> {};

template <typename ReturnType, typename ClassType, typename... Args>
struct function_traits<ReturnType (ClassType::*)(Args...)>
    : function_traits<ReturnType(Args...)> {};

template <typename ReturnType, typename ClassType, typename... Args>
struct function_traits<ReturnType (ClassType::*)(Args...) const>
    : function_traits<ReturnType(Args...)> {};

template <typename Callable>
struct function_traits : function_traits<decltype(&Callable::operator())> {};

template <typename T>
using remove_const_reference_t =
    std::remove_const_t<std::remove_reference_t<T>>;

template <size_t... Is>
auto make_tuple_from_sequence(std::index_sequence<Is...>)
    -> decltype(std::make_tuple(Is...)) {
  std::make_tuple(Is...);
}

template <size_t N>
constexpr auto make_tuple_from_sequence()
    -> decltype(make_tuple_from_sequence(std::make_index_sequence<N>{})) {
  return make_tuple_from_sequence(std::make_index_sequence<N>{});
}

namespace detail {
template <class Tuple, class F, std::size_t... Is>
void tuple_switch(const std::size_t i, Tuple &&t, F &&f,
                  std::index_sequence<Is...>) {
  (void)std::initializer_list<int>{
      (i == Is &&
       ((void)std::forward<F>(f)(std::integral_constant<size_t, Is>{}), 0))...};
}
} // namespace detail

template <class Tuple, class F>
inline void tuple_switch(const std::size_t i, Tuple &&t, F &&f) {
  constexpr auto N = std::tuple_size<std::remove_reference_t<Tuple>>::value;

  detail::tuple_switch(i, std::forward<Tuple>(t), std::forward<F>(f),
                       std::make_index_sequence<N>{});
}

template <int N, typename... Args>
using nth_type_of = std::tuple_element_t<N, std::tuple<Args...>>;

template <typename... Args>
using last_type_of = nth_type_of<sizeof...(Args) - 1, Args...>;
} // namespace rest_rpc

#endif // REST_RPC_META_UTIL_HPP
