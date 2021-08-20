#pragma once
#include <cstdint>

namespace rest_rpc {

enum class result_code : std::int16_t {
  OK = 0,
  FAIL = 1,
};

enum class error_code {
  OK,
  UNKNOWN,
  FAIL,
  TIMEOUT,
  CANCEL,
  BADCONNECTION,
};

enum class request_type : uint8_t { req_res, sub_pub };

struct message_type {
  std::uint64_t req_id;
  request_type req_type;
  std::shared_ptr<std::string> content;
};

#pragma pack(1)
struct rpc_header {
  uint32_t body_len;
  uint64_t req_id;
  request_type req_type;
};
#pragma pack()

static const size_t MAX_BUF_LEN = 1048576 * 10;
static const size_t HEAD_LEN = 13;
static const size_t INIT_BUF_SIZE = 2 * 1024;
} // namespace rest_rpc