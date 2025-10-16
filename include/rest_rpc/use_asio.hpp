#pragma once

#include <asio.hpp>
#ifdef CINATRA_ENABLE_SSL
#include <asio/ssl.hpp>
#endif
#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/detail/noncopyable.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

using tcp_socket = asio::ip::tcp::socket;
#ifdef CINATRA_ENABLE_SSL
using ssl_socket = asio::ssl::stream<asio::ip::tcp::socket>;
#endif

#include <string_view>
