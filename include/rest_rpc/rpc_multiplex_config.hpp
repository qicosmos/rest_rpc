#pragma once

#include <cstddef>

namespace rest_rpc {

struct multiplex_client_limits {
  size_t max_pending = 1024;
  size_t max_write_queue = 1024;
  size_t max_abandoned = 4096;
  size_t max_queued_bytes = 64 * 1024 * 1024;
  size_t max_body_size = 16 * 1024 * 1024;
};

struct multiplex_server_limits {
  size_t max_handlers = 1024;
  size_t max_response_queue = 1024;
  size_t max_queued_bytes = 64 * 1024 * 1024;
  size_t max_body_size = 16 * 1024 * 1024;
};

} // namespace rest_rpc
