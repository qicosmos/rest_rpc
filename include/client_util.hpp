#pragma once
#include <string_view>
#include "codec.h"

namespace rest_rpc {
	inline bool has_error(std::string_view result) {
		rpc_service::msgpack_codec codec;
		auto tp = codec.unpack<std::tuple<int>>(result.data(), result.size());

		return std::get<0>(tp) != 0;
	}

	template<typename T>
	inline T get_result(std::string_view result) {
		rpc_service::msgpack_codec codec;
		auto tp = codec.unpack<std::tuple<int>>(result.data(), result.size());
		if (std::get<0>(tp) != 0)
			throw std::logic_error("rpc error");

		auto err_tp = codec.unpack<std::tuple<int, T>>(result.data(), result.size());
		return std::get<1>(err_tp);
	}
}