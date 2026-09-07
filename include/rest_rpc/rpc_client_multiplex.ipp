#pragma once

#include <optional>
#include <stdexcept>

namespace rest_rpc {

class rpc_context;

template <typename R> class async_result {
public:
  using value_type = call_result<R>;

  async_result() = default;
  explicit async_result(asio::awaitable<value_type> operation)
      : operation_(std::move(operation)) {}

  async_result(async_result &&) noexcept = default;
  async_result &operator=(async_result &&) noexcept = default;
  async_result(const async_result &) = delete;
  async_result &operator=(const async_result &) = delete;

  [[nodiscard]] asio::awaitable<value_type> wait() {
    if (!operation_.valid()) {
      throw std::logic_error("async_result can only be awaited once");
    }
    return std::move(operation_);
  }

  bool valid() const noexcept { return operation_.valid(); }

private:
  asio::awaitable<value_type> operation_;
};

namespace detail {

template <typename Tuple> struct multiplex_call_parameters {
  using type = Tuple;
};

template <typename First, typename... Args>
struct multiplex_call_parameters<std::tuple<First, Args...>> {
  using type = std::conditional_t<
      std::is_same_v<std::remove_cvref_t<First>, rpc_context>,
      std::tuple<Args...>, std::tuple<First, Args...>>;
};

template <typename Tuple>
using multiplex_call_parameters_t =
    typename multiplex_call_parameters<Tuple>::type;

template <typename T> auto own_multiplex_arg(T &&value) {
  if constexpr (util::CharArray<T> ||
                std::is_same_v<std::remove_cvref_t<T>, std::string_view>) {
    return std::string(std::string_view(value));
  } else {
    return std::decay_t<T>(std::forward<T>(value));
  }
}

template <typename R> call_result<R> make_multiplex_result(rpc_errc ec) {
  call_result<R> result{};
  result.ec = ec;
  return result;
}

template <typename R>
asio::awaitable<call_result<R>> make_ready_multiplex_result(rpc_errc ec) {
  co_return make_multiplex_result<R>(ec);
}

template <typename R>
asio::awaitable<async_result<R>> make_ready_multiplex_sender(rpc_errc ec) {
  co_return async_result<R>{make_ready_multiplex_result<R>(ec)};
}

template <typename R, typename Session>
asio::awaitable<call_result<R>>
wait_multiplex_result(std::shared_ptr<multiplex_pending_response> pending,
                      std::shared_ptr<Session> session) {
  if (!pending->completed) {
    auto [wait_ec] =
        co_await pending->ready.async_wait(asio::as_tuple(asio::use_awaitable));
    (void)wait_ec;
    if (!pending->completed) {
      co_await asio::this_coro::reset_cancellation_state();
      co_await asio::co_spawn(session->get_executor(),
                              session->cancel_request(pending->seq_num),
                              asio::use_awaitable);
    }
  }

  auto result = make_multiplex_result<R>(pending->ec);
  if constexpr (!std::is_void_v<R>) {
    if (result.ec == rpc_errc::ok) {
      result.value = rpc_codec::unpack<R>(pending->body);
    }
  }
  co_return result;
}

template <typename Inner> struct multiplex_start_state {
  explicit multiplex_start_state(asio::any_io_executor executor)
      : ready(executor) {
    ready.expires_at(std::chrono::steady_clock::time_point::max());
  }

  asio::steady_timer ready;
  std::optional<Inner> sender;
  std::exception_ptr exception;
  bool completed = false;
};

template <typename Inner>
asio::awaitable<typename Inner::value_type>
await_multiplex_start(std::shared_ptr<multiplex_start_state<Inner>> state) {
  if (!state->completed) {
    auto [ec] =
        co_await state->ready.async_wait(asio::as_tuple(asio::use_awaitable));
    (void)ec;
  }
  if (!state->completed) {
    typename Inner::value_type result{};
    result.ec = rpc_errc::request_cancelled;
    co_return result;
  }
  if (state->exception) {
    std::rethrow_exception(state->exception);
  }
  co_return co_await state->sender->wait();
}

template <typename Inner>
Inner make_multiplex_start_result(
    std::shared_ptr<multiplex_start_state<Inner>> state) {
  auto executor = state->ready.get_executor();
  return Inner{asio::co_spawn(executor,
                              await_multiplex_start<Inner>(std::move(state)),
                              asio::use_awaitable)};
}

} // namespace detail

template <auto func, typename... Args>
asio::awaitable<
    async_result<return_type_t<function_return_type_t<decltype(func)>>>>
rpc_client::send_call(Args &&...args) {
  return send_call_for<func>(std::chrono::seconds(5),
                             std::forward<Args>(args)...);
}

template <auto func, typename Rep, typename Period, typename... Args>
asio::awaitable<
    async_result<return_type_t<function_return_type_t<decltype(func)>>>>
rpc_client::send_call_for(std::chrono::duration<Rep, Period> timeout,
                          Args &&...args) {
  using R = return_type_t<function_return_type_t<decltype(func)>>;
  using args_tuple = function_parameters_t<decltype(func)>;
  using call_args = detail::multiplex_call_parameters_t<args_tuple>;
  static_assert(std::is_constructible_v<call_args, Args...>,
                "called rpc function and arguments are not match");
  const auto steady_timeout =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout);
  if (steady_timeout <= std::chrono::steady_clock::duration::zero()) {
    return detail::make_ready_multiplex_sender<R>(rpc_errc::request_timeout);
  }
  auto owned_args =
      std::tuple{detail::own_multiplex_arg(std::forward<Args>(args))...};
  return send_call_owned<func>(steady_timeout, std::move(owned_args));
}

template <typename... Calls> auto rpc_client::send_calls(Calls &&...calls) {
  return std::tuple{start_multiplex_call(std::forward<Calls>(calls))...};
}

inline void rpc_client::set_multiplex_limits(multiplex_client_limits limits) {
  multiplex_limits_ = limits;
  if (multiplex_session_) {
    multiplex_session_->set_limits(limits);
  }
}

template <auto func, typename Tuple>
asio::awaitable<
    async_result<return_type_t<function_return_type_t<decltype(func)>>>>
rpc_client::send_call_owned(std::chrono::steady_clock::duration timeout,
                            Tuple owned_args) {
  using R = return_type_t<function_return_type_t<decltype(func)>>;

  auto buffer = std::apply(
      [this](auto &...args) { return get_buffer(std::move(args)...); },
      owned_args);
  std::string body;
  if (!buffer.empty()) {
    body.assign(buffer.data(), buffer.size());
  }

  co_return co_await asio::co_spawn(
      socket_->get_executor(),
      start_multiplex_request<func, R>(timeout, std::move(body)),
      asio::use_awaitable);
}

template <auto func, typename R>
asio::awaitable<async_result<R>>
rpc_client::start_multiplex_request(std::chrono::steady_clock::duration timeout,
                                    std::string body) {
  if (!select_mode(protocol_mode::multiplex)) {
    co_return async_result<R>{detail::make_ready_multiplex_result<R>(
        rpc_errc::protocol_mode_conflict)};
  }
  if (!multiplex_session_) {
    multiplex_session_ = std::make_shared<multiplex_session_t>(
        socket_, cross_ending_, multiplex_limits_);
  }

  rest_rpc_header header{};
  header.version = REST_RPC_PROTOCOL_V2;
  header.function_id = get_key<func>();

  auto pending = co_await asio::co_spawn(
      multiplex_session_->get_executor(),
      multiplex_session_->start_request(header, std::move(body), timeout),
      asio::use_awaitable);
  co_return async_result<R>{asio::co_spawn(
      multiplex_session_->get_executor(),
      detail::wait_multiplex_result<R>(std::move(pending), multiplex_session_),
      asio::use_awaitable)};
}

template <typename Outer> auto rpc_client::start_multiplex_call(Outer &&outer) {
  using outer_t = std::remove_cvref_t<Outer>;
  using inner_t = typename outer_t::value_type;
  auto state = std::make_shared<detail::multiplex_start_state<inner_t>>(
      socket_->get_executor());
  async_start(socket_->get_executor(), std::forward<Outer>(outer),
              [state](std::exception_ptr exception, inner_t sender) {
                state->exception = std::move(exception);
                state->sender.emplace(std::move(sender));
                state->completed = true;
                state->ready.cancel();
              });
  return detail::make_multiplex_start_result<inner_t>(std::move(state));
}

} // namespace rest_rpc
