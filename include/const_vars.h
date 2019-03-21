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

	static const size_t MAX_BUF_LEN = 1048576 * 10;
	static const size_t HEAD_LEN = 12;
	static const size_t INIT_BUF_SIZE = 2 * 1024;
}