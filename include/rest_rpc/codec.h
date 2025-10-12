#ifndef REST_RPC_CODEC_H_
#define REST_RPC_CODEC_H_

#include "traits.h"
#include <charconv>
#include <msgpack.hpp>

namespace rest_rpc {

struct msgpack_codec {
  template <typename... Args> inline static auto pack_args(Args &&...args) {
    if constexpr (sizeof...(Args) == 0) {
      return std::string_view{};
    } else if constexpr (sizeof...(Args) == 1 && util::is_basic_v<Args...>) {
      return pack_one(std::forward<Args>(args)...);
    } else {
      msgpack::sbuffer buffer(2 * 1024);
      msgpack::pack(buffer, std::forward_as_tuple(std::forward<Args>(args)...));
      return std::string(buffer.data(), buffer.size());
    }
  }

  template <typename T> inline static T unpack(std::string_view data) {
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
      try {
        static msgpack::unpacked msg;
        msgpack::unpack(msg, data.data(), data.size());
        return msg.get().as<T>();
      } catch (...) {
        throw std::invalid_argument("unpack failed: Args not match!");
      }
    }
  }

private:
  template <typename Arg> inline static auto pack_one(Arg &&arg) {
    if constexpr (util::CharArrayRef<Arg> || util::CharArray<Arg> ||
                  util::string<Arg>) {
      return std::string_view(std::forward<Arg>(arg));
    } else {
      return std::to_string(arg);
    }
  }
};

} // namespace rest_rpc

#endif // REST_RPC_CODEC_H_