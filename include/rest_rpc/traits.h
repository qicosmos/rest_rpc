#pragma once
#include <functional>
#include <memory>

namespace rest_rpc::util {
template <typename Function> struct function_traits;

template <typename Return, typename... Arguments>
struct function_traits<Return (*)(Arguments...)> {
  using parameters_type = std::tuple<std::remove_cvref_t<Arguments>...>;
  using return_type = Return;
};

template <typename Return, typename... Arguments>
struct function_traits<Return (*)(Arguments...) noexcept> {
  using parameters_type = std::tuple<std::remove_cvref_t<Arguments>...>;
  using return_type = Return;
};

template <typename Return, typename... Arguments>
struct function_traits<Return(Arguments...)> {
  using parameters_type = std::tuple<std::remove_cvref_t<Arguments>...>;
  using return_type = Return;
};

template <typename Return, typename... Arguments>
struct function_traits<Return(Arguments...) noexcept> {
  using parameters_type = std::tuple<std::remove_cvref_t<Arguments>...>;
  using return_type = Return;
};

template <typename This, typename Return, typename... Arguments>
struct function_traits<Return (This::*)(Arguments...)> {
  using parameters_type = std::tuple<std::remove_cvref_t<Arguments>...>;
  using return_type = Return;
  using class_type = This;
};

template <typename This, typename Return, typename... Arguments>
struct function_traits<Return (This::*)(Arguments...) noexcept> {
  using parameters_type = std::tuple<std::remove_cvref_t<Arguments>...>;
  using return_type = Return;
  using class_type = This;
};

template <typename This, typename Return, typename... Arguments>
struct function_traits<Return (This::*)(Arguments...) const> {
  using parameters_type = std::tuple<std::remove_cvref_t<Arguments>...>;
  using return_type = Return;
  using class_type = This;
};

template <typename This, typename Return, typename... Arguments>
struct function_traits<Return (This::*)(Arguments...) const noexcept> {
  using parameters_type = std::tuple<std::remove_cvref_t<Arguments>...>;
  using return_type = Return;
  using class_type = This;
};

template <typename Return> struct function_traits<Return (*)()> {
  using parameters_type = std::tuple<>;
  using return_type = Return;
};

template <typename Return> struct function_traits<Return (*)() noexcept> {
  using parameters_type = std::tuple<>;
  using return_type = Return;
};

template <typename Return> struct function_traits<Return (&)()> {
  using parameters_type = std::tuple<>;
  using return_type = Return;
};

template <typename Return> struct function_traits<Return (&)() noexcept> {
  using parameters_type = std::tuple<>;
  using return_type = Return;
};

template <typename Return> struct function_traits<Return()> {
  using parameters_type = std::tuple<>;
  using return_type = Return;
};

template <typename Return> struct function_traits<Return() noexcept> {
  using parameters_type = std::tuple<>;
  using return_type = Return;
};

template <typename This, typename Return>
struct function_traits<Return (This::*)()> {
  using parameters_type = std::tuple<>;
  using return_type = Return;
  using class_type = This;
};

template <typename This, typename Return>
struct function_traits<Return (This::*)() noexcept> {
  using parameters_type = std::tuple<>;
  using return_type = Return;
  using class_type = This;
};

template <typename This, typename Return>
struct function_traits<Return (This::*)() const> {
  using parameters_type = std::tuple<>;
  using return_type = Return;
  using class_type = This;
};

template <typename This, typename Return>
struct function_traits<Return (This::*)() const noexcept> {
  using parameters_type = std::tuple<>;
  using return_type = Return;
  using class_type = This;
};

// Support function object and lambda expression
template <class Function>
struct function_traits : function_traits<decltype(&Function::operator())> {};

template <typename Function>
using function_parameters_t =
    typename function_traits<std::remove_cvref_t<Function>>::parameters_type;

template <typename Function>
using last_parameters_type_t =
    std::tuple_element_t<std::tuple_size_v<function_parameters_t<Function>> - 1,
                         function_parameters_t<Function>>;

template <typename Function>
using function_return_type_t =
    typename function_traits<std::remove_cvref_t<Function>>::return_type;

template <typename Function>
using class_type_t =
    typename function_traits<std::remove_cvref_t<Function>>::class_type;

template <typename F, typename... Args>
struct is_invocable
    : std::is_constructible<
          std::function<void(std::remove_reference_t<Args>...)>,
          std::reference_wrapper<typename std::remove_reference<F>::type>> {};

template <typename F, typename... Args>
inline constexpr bool is_invocable_v = is_invocable<F, Args...>::value;

template <typename T> struct remove_first {
  using type = T;
};

template <class First, class... Second>
struct remove_first<std::tuple<First, Second...>> {
  using type = std::tuple<Second...>;
};

template <typename T> using remove_first_t = typename remove_first<T>::type;

template <bool has_conn, typename T> inline auto get_args() {
  if constexpr (has_conn) {
    using args_type = remove_first_t<T>;
    return args_type{};
  } else {
    return T{};
  }
}

template <typename Test, template <typename...> class Ref>
struct is_specialization : std::false_type {};

template <template <typename...> class Ref, typename... Args>
struct is_specialization<Ref<Args...>, Ref> : std::true_type {};

template <typename Test, template <typename...> class Ref>
inline constexpr bool is_specialization_v = is_specialization<Test, Ref>::value;

template <typename Type>
concept container = requires(Type container) {
  typename std::remove_cvref_t<Type>::value_type;
  container.size();
  container.begin();
  container.end();
};

template <typename Type>
constexpr bool is_char_t =
    std::is_same_v<Type, signed char> || std::is_same_v<Type, char> ||
    std::is_same_v<Type, unsigned char> || std::is_same_v<Type, wchar_t> ||
    std::is_same_v<Type, char16_t> || std::is_same_v<Type, char32_t>
#ifdef __cpp_lib_char8_t
    || std::is_same_v<Type, char8_t>
#endif
    ;

template <typename Type>
concept string = container<Type> && requires(Type container) {
  requires is_char_t<typename std::remove_cvref_t<Type>::value_type>;
  container.length();
  container.data();
};

template <typename T>
concept CharArrayRef = requires {
  requires std::is_array_v<std::remove_reference_t<T>> &&
               std::same_as<std::remove_extent_t<std::remove_cvref_t<T>>, char>;
};

template <typename T>
concept CharArray =
    std::is_array_v<std::remove_reference_t<T>> &&
    std::same_as<std::remove_extent_t<std::remove_cvref_t<T>>, char>;

template <typename T, typename...>
inline constexpr bool is_basic_v =
    std::is_fundamental_v<T> || string<T> || CharArray<T> || CharArrayRef<T>;
} // namespace rest_rpc::util
