#ifndef REST_RPC_CODEC_H_
#define REST_RPC_CODEC_H_

#include "traits.h"
#include <charconv>
#include <msgpack.hpp>

namespace user_codec {
struct rest_adl_tag {};
} // namespace user_codec

namespace rest_rpc {
namespace detail {
template <typename T, typename... Args>
struct has_user_pack : std::false_type {};

template <typename... Args>
struct has_user_pack<
    std::void_t<
        // AdlTag{} trigger ADL lookup user_codec namespace
        decltype(serialize(std::declval<user_codec::rest_adl_tag>(),
                           std::declval<Args>()...))>,
    Args...> : std::true_type {};

template <typename... Args>
inline constexpr bool has_user_pack_v = has_user_pack<void, Args...>::value;

} // namespace detail

struct rpc_codec {
  template <typename... Args> inline static auto pack_args(Args &&...args) {
    if constexpr (sizeof...(Args) == 0) {
      return std::string_view{};
    } else if constexpr (sizeof...(Args) == 1 && util::is_basic_v<Args...>) {
      return pack_one(std::forward<Args>(args)...);
    } else {
      if constexpr (detail::has_user_pack_v<Args...>) {
        return serialize(user_codec::rest_adl_tag{},
                         std::forward<Args>(args)...);
      } else {
        msgpack::sbuffer buffer(2 * 1024);
        if constexpr (sizeof...(Args) > 1) {
          msgpack::pack(buffer,
                        std::forward_as_tuple(std::forward<Args>(args)...));
        } else {
          msgpack::pack(buffer, std::forward<Args>(args)...);
        }

        return std::string(buffer.data(), buffer.size());
      }
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
      if constexpr (detail::has_user_pack_v<T>) {
        return deserialize<T>(user_codec::rest_adl_tag{}, data);
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