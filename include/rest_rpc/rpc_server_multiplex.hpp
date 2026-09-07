#pragma once

#include "logger.hpp"
#include "rest_rpc_protocol.hpp"
#include "rpc_multiplex_config.hpp"
#include "rpc_router.hpp"
#include "use_asio.hpp"
#include <asio/strand.hpp>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace rest_rpc::detail {

class multiplex_server_session
    : public std::enable_shared_from_this<multiplex_server_session> {
public:
  multiplex_server_session(tcp_socket &socket, rpc_router &router,
                           bool cross_ending, std::shared_ptr<void> owner,
                           std::function<void()> close_connection,
                           std::function<void()> touch_connection,
                           multiplex_server_limits limits)
      : socket_(socket), router_(router), cross_ending_(cross_ending),
        owner_(std::move(owner)),
        close_connection_(std::move(close_connection)),
        touch_connection_(std::move(touch_connection)), limits_(limits),
        executor_(asio::make_strand(socket_.get_executor())) {}

  asio::any_io_executor get_executor() const { return executor_; }

  asio::awaitable<void> run(rest_rpc_header first_header) {
    co_await accept_request(first_header);
    while (!stopped_) {
      rest_rpc_header header{};
      touch_connection_();
      auto [ec, size] = co_await asio::async_read(
          socket_, asio::buffer(&header, sizeof(header)),
          asio::as_tuple(asio::use_awaitable));
      (void)size;
      if (ec) {
        stop();
        co_return;
      }

      if (cross_ending_) {
        parse_recieved(header);
      }
      co_await accept_request(header);
    }
  }

private:
  struct request {
    rest_rpc_header header;
    std::string body;
  };

  struct response_frame {
    std::string bytes;
  };

  asio::awaitable<void> accept_request(rest_rpc_header header) {
    if (header.magic != REST_MAGIC_NUM ||
        header.version != REST_RPC_PROTOCOL_V2 ||
        header.serialize_type != REST_RPC_SERIALIZE_TYPE ||
        header.seq_num == 0 || header.attach_length != 0) {
      stop();
      co_return;
    }
    if (header.body_len > limits_.max_body_size) {
      stop();
      co_return;
    }

    request req{header, {}};
    try {
      req.body.resize(header.body_len);
    } catch (...) {
      stop();
      co_return;
    }
    if (header.body_len > 0) {
      touch_connection_();
      auto [ec, size] = co_await asio::async_read(
          socket_, asio::buffer(req.body), asio::as_tuple(asio::use_awaitable));
      (void)size;
      if (ec) {
        stop();
        co_return;
      }
    }

    if (header.msg_type != 0) {
      if (!enqueue_error(header, rpc_errc::invalid_req_type)) {
        stop();
      }
      co_return;
    }
    if (active_handlers_ >= limits_.max_handlers) {
      if (!enqueue_error(header, rpc_errc::queue_full)) {
        stop();
      }
      co_return;
    }

    ++active_handlers_;
    auto self = shared_from_this();
    std::weak_ptr<multiplex_server_session> weak_self = self;
    asio::co_spawn(
        get_executor(),
        [self = std::move(self),
         req = std::move(req)]() mutable -> asio::awaitable<void> {
          auto context = self->make_context(req.header);
          auto result = co_await self->router_.route_multiplex(
              req.header.function_id, req.body, context);
          if (result.suppress_response) {
            co_return;
          }
          if (!self->enqueue_response(req.header, result.ec, result.body)) {
            // An automatic response cannot report queue_full without another
            // response slot, so close the connection and wake all callers.
            self->stop();
          }
        },
        [weak_self](std::exception_ptr exception) {
          if (auto self = weak_self.lock()) {
            --self->active_handlers_;
            if (exception) {
              self->stop();
            }
          }
        });
  }

  rpc_context make_context(rest_rpc_header request_header) {
    std::weak_ptr<multiplex_server_session> weak_self = shared_from_this();
    return rpc_context{
        get_executor(),
        [weak_self,
         request_header](std::string body) -> asio::awaitable<std::error_code> {
          auto self = weak_self.lock();
          if (!self) {
            co_return make_error_code(rpc_errc::socket_closed);
          }
          auto executor = self->get_executor();
          co_return co_await asio::co_spawn(
              executor,
              [self = std::move(self), request_header,
               body = std::move(
                   body)]() mutable -> asio::awaitable<std::error_code> {
                if (self->stopped_) {
                  co_return make_error_code(rpc_errc::socket_closed);
                }

                if (!self->enqueue_response(request_header, rpc_errc::ok,
                                            body)) {
                  co_return make_error_code(rpc_errc::queue_full);
                }
                co_return std::error_code{};
              },
              asio::use_awaitable);
        }};
  }

  bool enqueue_error(const rest_rpc_header &request_header, rpc_errc ec) {
    return enqueue_response(request_header, ec, {});
  }

  bool enqueue_response(const rest_rpc_header &request_header,
                        rpc_errc response_ec, std::string_view response_body) {
    if (stopped_) {
      return false;
    }

    rest_rpc_header response_header{};
    response_header.version = request_header.version;
    response_header.serialize_type = request_header.serialize_type;
    response_header.msg_type = request_header.msg_type;
    response_header.function_id = request_header.function_id;
    response_header.seq_num = request_header.seq_num;
    response_header.body_len = response_body.size() + 1;

    const auto frame_size = sizeof(rest_rpc_header) + response_header.body_len;
    if (response_queue_.size() >= limits_.max_response_queue ||
        frame_size > limits_.max_queued_bytes ||
        queued_bytes_ > limits_.max_queued_bytes - frame_size) {
      return false;
    }

    auto wire_header = response_header;
    if (cross_ending_) {
      prepare_for_send(wire_header);
    }

    try {
      response_frame frame;
      frame.bytes.resize(frame_size);
      std::memcpy(frame.bytes.data(), &wire_header, sizeof(wire_header));
      frame.bytes[sizeof(rest_rpc_header)] = static_cast<char>(response_ec);
      if (!response_body.empty()) {
        std::memcpy(frame.bytes.data() + sizeof(rest_rpc_header) + 1,
                    response_body.data(), response_body.size());
      }
      response_queue_.push_back(std::move(frame));
      queued_bytes_ += frame_size;
    } catch (...) {
      return false;
    }
    start_writer();
    return true;
  }

  void start_writer() {
    if (writer_started_) {
      return;
    }
    writer_started_ = true;
    auto self = shared_from_this();
    asio::co_spawn(get_executor(), write_loop(std::move(self)), asio::detached);
  }

  static asio::awaitable<void>
  write_loop(std::shared_ptr<multiplex_server_session> self) {
    while (!self->response_queue_.empty() && !self->stopped_) {
      auto frame = std::move(self->response_queue_.front());
      self->response_queue_.pop_front();
      self->queued_bytes_ -= frame.bytes.size();
      self->touch_connection_();
      auto [ec, size] =
          co_await asio::async_write(self->socket_, asio::buffer(frame.bytes),
                                     asio::as_tuple(asio::use_awaitable));
      (void)size;
      if (ec) {
        self->writer_started_ = false;
        self->stop();
        co_return;
      }
    }
    self->writer_started_ = false;
  }

  void stop() {
    if (stopped_) {
      return;
    }
    stopped_ = true;
    response_queue_.clear();
    queued_bytes_ = 0;
    close_connection_();
  }

  tcp_socket &socket_;
  rpc_router &router_;
  bool cross_ending_;
  std::shared_ptr<void> owner_;
  std::function<void()> close_connection_;
  std::function<void()> touch_connection_;
  bool stopped_ = false;
  bool writer_started_ = false;
  size_t active_handlers_ = 0;
  size_t queued_bytes_ = 0;
  multiplex_server_limits limits_;
  asio::strand<asio::any_io_executor> executor_;
  std::deque<response_frame> response_queue_;
};

} // namespace rest_rpc::detail
