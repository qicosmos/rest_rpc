#pragma once
#include <iostream>

namespace rest_rpc {
struct null_logger_t {
  template <typename T> const null_logger_t &operator<<(T &&) const {
    return *this;
  }
};
struct cout_logger_t {
  template <typename T> const cout_logger_t &operator<<(T &&t) const {
    std::cout << std::forward<T>(t);
    return *this;
  }
  ~cout_logger_t() { std::cout << std::endl; }
};
struct cerr_logger_t {
  template <typename T> const cerr_logger_t &operator<<(T &&t) const {
    std::cerr << std::forward<T>(t);
    return *this;
  }
  ~cerr_logger_t() { std::cerr << std::endl; }
};

constexpr inline rest_rpc::null_logger_t NULL_LOGGER;
} // namespace rest_rpc

#ifdef REST_LOG_ERROR
#else
#define REST_LOG_ERROR                                                         \
  rest_rpc::cerr_logger_t {}
#endif

#ifdef REST_LOG_WARNING
#else
#ifndef NDEBUG
#define REST_LOG_WARNING                                                       \
  rest_rpc::cerr_logger_t {}
#else
#define REST_LOG_WARNING rest_rpc::NULL_LOGGER
#endif
#endif

#ifdef REST_LOG_INFO
#else
#ifndef NDEBUG
#define REST_LOG_INFO                                                          \
  rest_rpc::cout_logger_t {}
#else
#define REST_LOG_INFO rest_rpc::NULL_LOGGER
#endif
#endif

#ifdef REST_LOG_DEBUG
#else
#ifndef NDEBUG
#define REST_LOG_DEBUG                                                         \
  rest_rpc::cout_logger_t {}
#else
#define REST_LOG_DEBUG rest_rpc::NULL_LOGGER
#endif
#endif

#ifdef REST_LOG_TRACE
#else
#ifndef NDEBUG
#define REST_LOG_TRACE                                                         \
  rest_rpc::cout_logger_t {}
#else
#define REST_LOG_TRACE rest_rpc::NULL_LOGGER
#endif
#endif