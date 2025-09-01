#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

namespace rest_rpc {
namespace detail {
template <auto Func>
constexpr std::string_view qualified_name_of_impl() noexcept {
#ifdef _MSC_VER
  constexpr std::size_t suffix_size{16};
  constexpr std::string_view keyword{
      "rest_rpc::detail::qualified_name_of_impl<"};
  constexpr std::string_view signature{__FUNCSIG__};
  constexpr std::string_view anonymous_namespace{"`anonymous-namespace'::"};
#elif defined(__clang__)
  constexpr std::size_t suffix_size{1};
  constexpr std::string_view keyword{"[Func = "};
  constexpr std::string_view signature{__PRETTY_FUNCTION__};
  constexpr std::string_view anonymous_namespace{"(anonymous namespace)::"};
#elif defined(__GNUC__)
  constexpr std::size_t suffix_size{1};
  constexpr std::string_view keyword{"Func = "};
  constexpr std::string_view signature{__PRETTY_FUNCTION__};
  constexpr std::string_view anonymous_namespace{"{anonymous}::"};
#else
#error "Unsupported compiler."
#endif
  // Skips the possible '&' token for GCC and Clang.
  constexpr auto prefix_size = signature.find(keyword) + keyword.size();
  constexpr auto additional_size = signature[prefix_size] == '&' ? 1 : 0;
  constexpr auto intermediate = signature.substr(
      prefix_size + additional_size,
      signature.size() - prefix_size - additional_size - suffix_size);

  constexpr std::string_view result = intermediate;
  constexpr size_t rpos = result.rfind(anonymous_namespace);
  if constexpr (rpos != std::string_view::npos) {
    constexpr std::string_view str =
        result.substr(rpos + anonymous_namespace.size());
    constexpr size_t right = str.find('(');
    if constexpr (right != std::string_view::npos) {
      return str.substr(0, right);
    } else {
      return str;
    }
  } else {
    constexpr size_t left = result.find("l ") + 2;
    constexpr size_t right = result.find('(');
    if constexpr (left != std::string_view::npos) {
      if constexpr (right != std::string_view::npos) {
        return result.substr(left, right - left);
      } else {
        return result;
      }
    } else {
      return result;
    }
  }
}
} // namespace detail

template <auto Func> struct qualified_name_of {
  static constexpr auto value = detail::qualified_name_of_impl<Func>();
};

template <auto Func>
inline constexpr auto &&qualified_name_of_v = qualified_name_of<Func>::value;
} // namespace rest_rpc

namespace rest_rpc {
template <size_t N>
constexpr std::string_view
string_view_array_has(const std::array<std::string_view, N> &array,
                      std::string_view value) {
  for (const auto &v : array) {
    if (value.find(v) == 0)
      return v;
  }
  return std::string_view{""};
}

template <auto func> constexpr std::string_view get_func_name() {
  constexpr std::array func_style_array{
      std::string_view{"__cdecl "},    std::string_view{"__clrcall "},
      std::string_view{"__stdcall "},  std::string_view{"__fastcall "},
      std::string_view{"__thiscall "}, std::string_view{"__vectorcall "}};
  constexpr auto qualified_name = std::string_view{qualified_name_of_v<func>};
  constexpr auto func_style =
      string_view_array_has(func_style_array, qualified_name);
  if constexpr (func_style.length() > 0) {
    return std::string_view{qualified_name.data() + func_style.length(),
                            qualified_name.length() - func_style.length()};
  }
  return qualified_name;
};
} // namespace rest_rpc
