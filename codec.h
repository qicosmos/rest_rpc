#ifndef REST_RPC_CODEC_H_
#define REST_RPC_CODEC_H_

#include <msgpack.hpp>

namespace rest_rpc {
namespace rpc_service {

struct msgpack_codec {
  using buffer_type = std::vector<char>;
  class buffer_t {
   public:
    buffer_t() : buffer_t(0) {}

    explicit buffer_t(size_t len) : buffer_(len, 0), offset_(0) {}

    buffer_t(buffer_t const&) = default;
    buffer_t(buffer_t&&) = default;
    buffer_t& operator=(buffer_t const&) = default;
    buffer_t& operator=(buffer_t&&) = default;

    void write(char const* data, size_t length) {
      if (buffer_.size() - offset_ < length) buffer_.resize(length + offset_);

      std::memcpy(buffer_.data() + offset_, data, length);
      offset_ += length;
    }

    std::vector<char> release() const noexcept { return std::move(buffer_); }

   private:
    std::vector<char> buffer_;
    size_t offset_;
  };

  template<typename... Args>
  static buffer_type pack_args(Args&&... args) {
    buffer_t buffer;
    auto args_tuple = std::make_tuple(std::forward<Args>(args)...);
    msgpack::pack(buffer, args_tuple);
    return buffer.release();
  }

  template<typename Arg, typename... Args,
           typename = typename std::enable_if<std::is_enum<Arg>::value>::type>
  static std::string pack_args_str(Arg arg, Args&&... args) {
    buffer_t buffer;
    auto args_tuple = std::make_tuple((int)arg, std::forward<Args>(args)...);
    msgpack::pack(buffer, args_tuple);
    auto ret = buffer.release();
    return std::string(ret.data(), ret.size());
  }

  template<typename T>
  buffer_type pack(T&& t) const {
    buffer_t buffer;
    msgpack::pack(buffer, std::forward<T>(t));
    return buffer.release();
  }

  template<typename T>
  T unpack(char const* data, size_t length) {
    try {
      msgpack::unpack(&msg_, data, length);
      return msg_.get().as<T>();
    } catch (...) { throw std::invalid_argument("Args not match!"); }
  }

 private:
  msgpack::unpacked msg_;
};
}  // namespace rpc_service
}  // namespace rest_rpc

#endif  // REST_RPC_CODEC_H_