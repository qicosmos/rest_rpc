#pragma once

#include "rpc_server_multiplex.hpp"

namespace rest_rpc {

inline asio::awaitable<void>
rpc_connection::start_multiplex(rest_rpc_header header) {
  auto self = shared_from_this();
  std::weak_ptr<rpc_connection> weak_self = self;
  auto session = std::make_shared<detail::multiplex_server_session>(
      socket_, router_, cross_ending_, std::move(self),
      [weak_self] {
        if (auto connection = weak_self.lock()) {
          connection->close();
        }
      },
      [weak_self] {
        if (auto connection = weak_self.lock()) {
          connection->set_last_time();
        }
      },
      multiplex_limits_);
  co_await asio::co_spawn(session->get_executor(), session->run(header),
                          asio::use_awaitable);
}

} // namespace rest_rpc
