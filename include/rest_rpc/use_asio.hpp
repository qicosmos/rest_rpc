#pragma once

#include <asio.hpp>
#ifdef CINATRA_ENABLE_SSL
#include <asio/ssl.hpp>
#endif
#include <asio/detail/noncopyable.hpp>
#include <asio/steady_timer.hpp>

using tcp_socket = asio::ip::tcp::socket;
#ifdef CINATRA_ENABLE_SSL
using ssl_socket = asio::ssl::stream<asio::ip::tcp::socket>;
#endif

#if __cplusplus > 201402L
#include <string_view>
using string_view = std::string_view;
#else
#include "string_view.hpp"
using namespace nonstd;
#endif

#ifdef CINATRA_ENABLE_SSL
#if __cplusplus > 201402L
#if defined(__GNUC__)
#if __GNUC__ < 8
#include <experimental/filesystem>
namespace rpcfs = std::experimental::filesystem;
#else
#include <filesystem>
namespace rpcfs = std::filesystem;
#endif
#else
#include <boost/filesystem.hpp>
namespace rpcfs = boost::filesystem;
#endif
#else
#include <boost/filesystem.hpp>
namespace rpcfs = boost::filesystem;
#endif
#endif
