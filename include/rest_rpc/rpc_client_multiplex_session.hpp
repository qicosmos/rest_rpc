#pragma once

#include "error_code.h"
#include "logger.hpp"
#include "rest_rpc_protocol.hpp"
#include "rpc_multiplex_config.hpp"
#include "use_asio.hpp"
#include <algorithm>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>

namespace rest_rpc::detail {

struct multiplex_pending_response {
  explicit multiplex_pending_response(asio::any_io_executor executor,
                                      uint64_t id)
      : ready(executor), deadline(executor), seq_num(id) {
    ready.expires_at(std::chrono::steady_clock::time_point::max());
  }

  void complete(rpc_errc error, std::string response_body = {}) {
    if (completed) {
      return;
    }

    completed = true;
    ec = error;
    body = std::move(response_body);
    deadline.cancel();
    ready.cancel();
  }

  asio::steady_timer ready;
  asio::steady_timer deadline;
  uint64_t seq_num;
  rpc_errc ec = rpc_errc::ok;
  std::string body;
  bool completed = false;
};

template <typename Transport>
class multiplex_client_session
    : public std::enable_shared_from_this<multiplex_client_session<Transport>> {
public:
  using pending_ptr = std::shared_ptr<multiplex_pending_response>;

  multiplex_client_session(std::shared_ptr<Transport> transport,
                           bool cross_ending, multiplex_client_limits limits)
      : transport_(std::move(transport)), cross_ending_(cross_ending),
        limits_(limits),
        executor_(asio::make_strand(transport_->get_executor())) {}

  asio::any_io_executor get_executor() const { return executor_; }

  asio::awaitable<pending_ptr>
  start_request(rest_rpc_header header, std::string body,
                std::chrono::steady_clock::duration timeout) {
    co_await asio::dispatch(get_executor(), asio::use_awaitable);

    if (timeout <= std::chrono::steady_clock::duration::zero()) {
      auto pending =
          std::make_shared<multiplex_pending_response>(get_executor(), 0);
      pending->complete(rpc_errc::request_timeout);
      co_return pending;
    }
    auto pending =
        std::make_shared<multiplex_pending_response>(get_executor(), 0);
    if (stopped_ || transport_->has_closed_) {
      pending->complete(rpc_errc::socket_closed);
      co_return pending;
    }
    if (body.size() > limits_.max_body_size) {
      pending->complete(rpc_errc::message_too_large);
      co_return pending;
    }
    const auto frame_size = sizeof(rest_rpc_header) + body.size();
    if (pending_.size() >= limits_.max_pending ||
        write_queue_.size() >= limits_.max_write_queue ||
        frame_size > limits_.max_queued_bytes ||
        queued_bytes_ > limits_.max_queued_bytes - frame_size) {
      pending->complete(rpc_errc::queue_full);
      co_return pending;
    }

    const auto seq = next_sequence();
    if (seq == 0) {
      stop(rpc_errc::protocol_error);
      pending->complete(rpc_errc::protocol_error);
      co_return pending;
    }
    pending->seq_num = seq;

    header.magic = REST_MAGIC_NUM;
    header.version = REST_RPC_PROTOCOL_V2;
    header.serialize_type = REST_RPC_SERIALIZE_TYPE;
    header.seq_num = seq;
    header.body_len = body.size();

    rest_rpc_header wire_header = header;
    if (cross_ending_) {
      prepare_for_send(wire_header);
    }

    write_frame frame;
    frame.seq_num = seq;
    frame.bytes.resize(frame_size);
    std::memcpy(frame.bytes.data(), &wire_header, sizeof(rest_rpc_header));
    if (!body.empty()) {
      std::memcpy(frame.bytes.data() + sizeof(rest_rpc_header), body.data(),
                  body.size());
    }

    // The pending entry must be visible before the complete frame is queued.
    pending_.emplace(seq, pending);
    try {
      write_queue_.push_back(std::move(frame));
      queued_bytes_ += frame_size;
      start_deadline(pending, timeout);
      start_read_loop();
      start_write_loop();
    } catch (...) {
      pending_.erase(seq);
      erase_queued(seq);
      throw;
    }
    co_return pending;
  }

  asio::awaitable<void> shutdown(rpc_errc ec = rpc_errc::socket_closed) {
    co_await asio::dispatch(get_executor(), asio::use_awaitable);
    stop(ec);
  }

  void request_shutdown(rpc_errc ec = rpc_errc::socket_closed) {
    auto self = this->shared_from_this();
    asio::dispatch(get_executor(),
                   [self = std::move(self), ec] { self->stop(ec); });
  }

  void set_limits(multiplex_client_limits limits) {
    auto self = this->shared_from_this();
    asio::dispatch(get_executor(), [self = std::move(self), limits] {
      self->limits_ = limits;
    });
  }

private:
  struct write_frame {
    uint64_t seq_num = 0;
    std::string bytes;
  };

  uint64_t next_sequence() {
    if (next_seq_ == std::numeric_limits<uint64_t>::max()) {
      return 0;
    }
    return ++next_seq_;
  }

  void start_read_loop() {
    if (read_started_) {
      return;
    }
    read_started_ = true;
    try {
      auto self = this->shared_from_this();
      asio::co_spawn(get_executor(), read_loop(std::move(self)),
                     asio::detached);
    } catch (...) {
      read_started_ = false;
      throw;
    }
  }

  void start_write_loop() {
    if (write_started_) {
      return;
    }
    write_started_ = true;
    try {
      auto self = this->shared_from_this();
      asio::co_spawn(get_executor(), write_loop(std::move(self)),
                     asio::detached);
    } catch (...) {
      write_started_ = false;
      throw;
    }
  }

  void start_deadline(const pending_ptr &pending,
                      std::chrono::steady_clock::duration timeout) {
    pending->deadline.expires_after(timeout);
    pending->deadline.async_wait([self = this->shared_from_this(),
                                  seq = pending->seq_num](std::error_code ec) {
      if (!ec) {
        self->finish(seq, rpc_errc::request_timeout);
      }
    });
  }

  static asio::awaitable<void>
  write_loop(std::shared_ptr<multiplex_client_session> self) {
    while (!self->write_queue_.empty() && !self->stopped_) {
      auto frame = std::move(self->write_queue_.front());
      self->write_queue_.pop_front();
      self->queued_bytes_ -= frame.bytes.size();
      if (!self->pending_.contains(frame.seq_num)) {
        continue;
      }
      auto [ec, size] = co_await asio::async_write(
          self->transport_->impl_, asio::buffer(frame.bytes),
          asio::as_tuple(asio::use_awaitable));
      (void)size;
      if (ec) {
        self->write_started_ = false;
        self->stop(rpc_errc::write_error);
        co_return;
      }
    }
    self->write_started_ = false;
  }

  static asio::awaitable<void>
  read_loop(std::shared_ptr<multiplex_client_session> self) {
    while (!self->stopped_) {
      rest_rpc_header header{};
      auto [head_ec, head_size] = co_await asio::async_read(
          self->transport_->impl_, asio::buffer(&header, sizeof(header)),
          asio::as_tuple(asio::use_awaitable));
      (void)head_size;
      if (head_ec) {
        self->stop(rpc_errc::read_error);
        co_return;
      }

      if (self->cross_ending_) {
        parse_recieved(header);
      }
      if (header.magic != REST_MAGIC_NUM ||
          header.version != REST_RPC_PROTOCOL_V2 ||
          header.serialize_type != REST_RPC_SERIALIZE_TYPE ||
          header.msg_type != 0 || header.seq_num == 0 ||
          header.attach_length != 0) {
        self->stop(rpc_errc::protocol_error);
        co_return;
      }
      if (header.body_len == 0 ||
          header.body_len > self->limits_.max_body_size) {
        self->stop(header.body_len == 0 ? rpc_errc::protocol_error
                                        : rpc_errc::message_too_large);
        co_return;
      }

      std::string body;
      try {
        body.resize(header.body_len);
      } catch (...) {
        self->stop(rpc_errc::queue_full);
        co_return;
      }
      auto [body_ec, body_size] =
          co_await asio::async_read(self->transport_->impl_, asio::buffer(body),
                                    asio::as_tuple(asio::use_awaitable));
      (void)body_size;
      if (body_ec) {
        self->stop(rpc_errc::read_error);
        co_return;
      }

      auto response_ec =
          static_cast<rpc_errc>(static_cast<int8_t>(body.front()));
      body.erase(body.begin());
      self->finish(header.seq_num, response_ec, std::move(body));
    }
  }

  void finish(uint64_t seq_num, rpc_errc ec, std::string body = {}) {
    auto node = pending_.extract(seq_num);
    if (node.empty()) {
      // Sequence numbers are never reused within a session. Unknown, late and
      // duplicate responses can all be discarded without tracking old requests.
      return;
    }
    if (ec == rpc_errc::request_timeout) {
      erase_queued(seq_num);
    }
    node.mapped()->complete(ec, std::move(body));
  }

  void erase_queued(uint64_t seq_num) {
    auto it = std::find_if(write_queue_.begin(), write_queue_.end(),
                           [seq_num](const write_frame &frame) {
                             return frame.seq_num == seq_num;
                           });
    if (it != write_queue_.end()) {
      queued_bytes_ -= it->bytes.size();
      write_queue_.erase(it);
    }
  }

  void stop(rpc_errc ec) {
    if (stopped_) {
      return;
    }
    stopped_ = true;
    read_started_ = false;
    write_started_ = false;
    write_queue_.clear();
    queued_bytes_ = 0;

    auto pending = std::move(pending_);
    pending_.clear();
    for (auto &[seq, response] : pending) {
      (void)seq;
      response->complete(ec);
    }

    std::error_code ignored;
    self_close(ignored);
  }

  void self_close(std::error_code &ignored) {
    transport_->impl_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    transport_->impl_.close(ignored);
    transport_->has_closed_ = true;
  }

  std::shared_ptr<Transport> transport_;
  bool cross_ending_ = false;
  bool read_started_ = false;
  bool write_started_ = false;
  bool stopped_ = false;
  uint64_t next_seq_ = 0;
  size_t queued_bytes_ = 0;
  multiplex_client_limits limits_;
  asio::strand<asio::any_io_executor> executor_;
  std::unordered_map<uint64_t, pending_ptr> pending_;
  std::deque<write_frame> write_queue_;
};

} // namespace rest_rpc::detail
