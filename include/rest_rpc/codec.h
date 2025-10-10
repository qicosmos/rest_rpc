#ifndef REST_RPC_CODEC_H_
#define REST_RPC_CODEC_H_

#include "traits.h"
#include <charconv>
#include <msgpack.hpp>

namespace rest_rpc {
namespace rpc_service {

template <typename Arg> auto pack_one(Arg &&arg) {
  if constexpr (util::CharArrayRef<Arg> || util::CharArray<Arg> ||
                util::string<Arg>) {
    return std::string_view(std::forward<Arg>(arg));
  } else {
    return std::to_string(arg);
  }
}

using buffer_type = msgpack::sbuffer;
struct msgpack_codec {
  const static size_t init_size = 2 * 1024;

  template <typename... Args> static auto pack_args(Args &&...args) {
    if constexpr (sizeof...(Args) == 0) {
      return std::string_view{};
    } else if constexpr (sizeof...(Args) == 1 && util::is_basic_v<Args...>) {
      return pack_one(std::forward<Args>(args)...);
    } else {
      buffer_type buffer(init_size);
      msgpack::pack(buffer, std::forward_as_tuple(std::forward<Args>(args)...));
      return std::string(buffer.data(), buffer.size());
    }
  }

  template <typename Arg> static std::string pack_to_string(Arg &&arg) {
    buffer_type buffer(init_size);
    msgpack::pack(buffer, arg);
    return std::string(buffer.data(), buffer.size());
  }

  template <typename Arg, typename... Args,
            typename = typename std::enable_if<std::is_enum<Arg>::value>::type>
  static std::string pack_args_str(Arg arg, Args &&...args) {
    buffer_type buffer(init_size);
    msgpack::pack(buffer,
                  std::forward_as_tuple((int)arg, std::forward<Args>(args)...));
    return std::string(buffer.data(), buffer.size());
  }

  template <typename T> buffer_type pack(T &&t) const {
    buffer_type buffer;
    msgpack::pack(buffer, std::forward<T>(t));
    return buffer;
  }

  template <typename T> T unpack(char const *data, size_t length) {
    try {
      msgpack::unpack(msg_, data, length);
      return msg_.get().as<T>();
    } catch (...) {
      throw std::invalid_argument("unpack failed: Args not match!");
    }
  }

  template <typename T> T unpack(std::string_view data) {
    if constexpr (std::is_fundamental_v<T>) {
      T t;
      auto r = std::from_chars(data.data(), data.data() + data.size(), t);
      if (r.ec != std::errc()) {
        throw std::invalid_argument("unpack failed: Args not match!");
      }
      return t;
    } else if constexpr (std::is_same_v<std::string, T>) {
      return std::string(data);
    } else if constexpr (std::is_same_v<std::string_view, T>) {
      return data;
    } else {
      return unpack<T>(data.data(), data.size());
    }
  }

private:
  msgpack::unpacked msg_;
};
} // namespace rpc_service
} // namespace rest_rpc

#endif // REST_RPC_CODEC_H_