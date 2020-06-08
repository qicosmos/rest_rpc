#pragma once

#if defined(ASIO_STANDALONE)
//MSVC : define environment path 'ASIO_STANDALONE_INCLUDE', e.g. 'E:\bdlibs\asio-1.10.6\include'

#include <asio.hpp>
#ifdef CINATRA_ENABLE_SSL
#include <asio/ssl.hpp>
#endif
#include <asio/steady_timer.hpp>
#include <asio/detail/noncopyable.hpp>
namespace boost
{
	namespace asio
	{
		using namespace ::asio;
	}
	namespace system {
		using ::std::error_code;
	}
}
#else
#include <boost/asio.hpp>
#ifdef CINATRA_ENABLE_SSL
#include <boost/asio/ssl.hpp>
#endif
#include <boost/asio/steady_timer.hpp>
using namespace boost;
using tcp_socket = boost::asio::ip::tcp::socket;
#ifdef CINATRA_ENABLE_SSL
using ssl_socket = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;
#endif
#endif

#if __cplusplus > 201402L
#include <string_view>
using string_view = std::string_view;
#else
#ifdef ASIO_STANDALONE
#include "string_view.hpp"
using namespace nonstd;
#else
#include <boost/utility/string_view.hpp>
using string_view = boost::string_view;
#endif
#endif

#if __cplusplus > 201402L
#if defined (__GNUC__)
#if __GNUC__ < 8
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else
#include <filesystem>
namespace fs = std::filesystem;
#endif
#else
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;
#endif
#else
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;
#endif
