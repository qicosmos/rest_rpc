#pragma once
#include <cstdint>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#ifdef __APPLE__
#else
inline uint64_t htonll(uint64_t value) {
  return ((uint64_t)htonl(value & 0xFFFFFFFF) << 32) | htonl(value >> 32);
}

inline uint64_t ntohll(uint64_t value) {
  return ((uint64_t)ntohl(value & 0xFFFFFFFF) << 32) | ntohl(value >> 32);
}
#endif
#endif

namespace rest_rpc {
inline constexpr uint8_t REST_MAGIC_NUM = 39;
struct rest_rpc_header {
  uint8_t magic;
  uint8_t version;
  uint8_t serialize_type;
  uint8_t msg_type;
  uint32_t function_id;
  uint64_t seq_num;
  uint64_t body_len;
  uint64_t attach_length;
};

inline void prepare_for_send(rest_rpc_header &header) {
  header.function_id = htonl(header.function_id);
  header.seq_num = htonll(header.seq_num);
  header.body_len = htonll(header.body_len);
  header.attach_length = htonll(header.attach_length);
}

inline void parse_recieved(rest_rpc_header &header) {
  header.function_id = ntohl(header.function_id);
  header.seq_num = ntohll(header.seq_num);
  header.body_len = ntohll(header.body_len);
  header.attach_length = ntohll(header.attach_length);
}
} // namespace rest_rpc