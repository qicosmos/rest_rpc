#ifndef REST_RPC_CODEC_H_
#define REST_RPC_CODEC_H_

#include <msgpack.hpp>

namespace rest_rpc {
namespace rpc_service {

using buffer_type = msgpack::sbuffer;
struct msgpack_codec {
  const static size_t init_size = 2 * 1024;

  template <typename... Args> static buffer_type pack_args(Args &&...args) {
    buffer_type buffer(init_size);
    msgpack::pack(buffer, std::forward_as_tuple(std::forward<Args>(args)...));
    return buffer;
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

private:
  msgpack::unpacked msg_;
};
} // namespace rpc_service
} // namespace rest_rpc

#endif // REST_RPC_CODEC_H_