#pragma once
#include <cstdint>
#include <memory>
#include <string>

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

static const uint8_t MAGIC_NUM = 39;
#pragma pack(4)
struct rpc_header {
  uint8_t magic;
  request_type req_type;
  uint32_t body_len;
  uint64_t req_id;
  uint32_t func_id;
};
#pragma pack()

static const size_t MAX_BUF_LEN = 1048576 * 10;
static const size_t HEAD_LEN = sizeof(rpc_header);
static const size_t INIT_BUF_SIZE = 2 * 1024;
} // namespace rest_rpc