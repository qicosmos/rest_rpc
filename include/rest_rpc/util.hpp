#pragma once
#include "function_name.h"
#include "md5.hpp"

namespace rest_rpc {
template <auto func> constexpr uint32_t get_key() {
  constexpr auto name = get_func_name<func>();
  constexpr uint32_t key = MD5::MD5Hash32(name.data(), (uint32_t)name.length());
  return key;
}
} // namespace rest_rpc