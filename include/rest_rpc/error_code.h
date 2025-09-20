#pragma once
#include <string>
#include <system_error>

namespace rest_rpc {
enum class rpc_errc : std::int8_t {
  ok = 0,
  no_such_function,
  no_such_key,
  invalid_req_type,
  function_exception,
  function_unknown_exception,
  invalid_argument,
  write_error,
  read_error,
  socket_closed,
  resolve_timeout,
  connection_timeout,
  request_timeout,
  protocol_error
};

class rpc_error_category : public std::error_category {
public:
  const char *name() const noexcept override { return "rest_rpc_error"; }

  std::string message(int ev) const override {
    switch (static_cast<rpc_errc>(ev)) {
    case rpc_errc::ok:
      return "ok";
    case rpc_errc::no_such_function:
      return "no such function";
    case rpc_errc::no_such_key:
      return "resolve failed";
    case rpc_errc::invalid_req_type:
      return "invalid request type";
    case rpc_errc::function_exception:
      return "logic function exception happend";
    case rpc_errc::function_unknown_exception:
      return "unknown function exception happend";
    case rpc_errc::invalid_argument:
      return "invalid argument";
    case rpc_errc::write_error:
      return "write failed";
    case rpc_errc::read_error:
      return "read failed";
    case rpc_errc::socket_closed:
      return "socket closed";
    case rpc_errc::resolve_timeout:
      return "resolve timeout";
    case rpc_errc::connection_timeout:
      return "connect timeout";
    case rpc_errc::request_timeout:
      return "request timeout";
    case rpc_errc::protocol_error:
      return "protocol error";
    default:
      return "unknown error";
    }
  }
};

inline rest_rpc::rpc_error_category &category() {
  static rest_rpc::rpc_error_category instance;
  return instance;
}

inline std::error_code make_error_code(rpc_errc e) {
  return {static_cast<int>(e), category()};
}

inline bool operator==(const std::error_code &code, rpc_errc ec) {
  return code.value() == (int)ec;
}
} // namespace rest_rpc