#define DOCTEST_CONFIG_IMPLEMENT

#include "doctest/doctest.h"
#include <asio/any_completion_handler.hpp>
#include <rest_rpc/rpc_client.hpp>
#include <rest_rpc/rpc_server.hpp>
#include <rest_rpc/traits.h>

using namespace rest_rpc;

struct person {
  size_t id;
  std::string name;
  size_t age;
};

person get_person(person p) { return p; }

person modify_person(person p, std::string name) {
  p.name = name;
  return p;
}

struct dummy {
  int add(int a, int b) { return a + b; }

  void foo(std::string str) { std::cout << str << "\n"; }

  std::string echo(std::string val) { return val; }

  int round1(int i) { return i; }
  asio::awaitable<void> no_arg_coro() {
    std::cout << "no args\n";
    co_return;
  }

  asio::awaitable<std::string> no_arg_coro1() {
    std::cout << "no args\n";
    co_return "test";
  }

  asio::awaitable<std::string> echo_coro(std::string str) { co_return str; }
  asio::awaitable<int> add_coro(int a, int b) { co_return a + b; }
};

int add(int a, int b) { return a + b; }

asio::awaitable<int> add_coro(int a, int b) { co_return a + b; }

void foo(std::string str) { std::cout << str << "\n"; }

int round1(int i) { return i; }

std::string_view echo_sv(std::string_view str) { return str; }

template <auto func> asio::awaitable<void> response(auto ctx) {
  auto ec = co_await ctx.template response_s<func>("test");
  if (ec) {
    REST_LOG_ERROR << "response error: " << ec.message();
  }
  ec = co_await ctx.template response_s<func>("test");
  REST_LOG_ERROR << ec.message();
  CHECK(ec);

  ec = co_await ctx.response("test");
  REST_LOG_ERROR << ec.message();
  CHECK(ec);
}

std::string echo(std::string str) { return str; }

asio::awaitable<std::string> echo_coro(std::string str) { co_return str; }

void no_arg() { std::cout << "no args\n"; }

void exception_func() { throw std::invalid_argument("invalid"); }

void unknown_exception_func() { throw 1; }

asio::awaitable<void> no_arg_coro() {
  std::cout << "no args\n";
  co_return;
}

asio::awaitable<std::string> no_arg_coro1() {
  std::cout << "no args\n";
  co_return "test";
}

asio::awaitable<std::string> delay_response2(std::string_view str) {
  auto coro = []() -> asio::awaitable<void> {
    rpc_context ctx; // rpc_context init will be failed, it should be defined in
                     // io thread.
    auto ret = co_await ctx.response("test");
    REST_LOG_INFO << ret.message();
    CHECK(ret);
  };
  sync_wait(get_global_executor(), coro());

  rpc_context ctx; // right, created in io thread.
  co_await ctx.response(str);
  auto ret = co_await ctx.response(str);
  CHECK(ret);
  
  // this return value is meaningless, because it will response later, the
  // return type is important for client, so just return an empty value here.
  co_return "";
}

std::string delay_response3(std::string str) {
  rpc_context ctx;
  std::thread thd([ctx = std::move(ctx), str]() mutable {
    sync_wait(ctx.get_executor(), ctx.response(std::move(str)));
  });
  thd.detach();

  return "";
}

TEST_CASE("test delay response") {
  using T = return_type_t<int>;
  rpc_server server("127.0.0.1:9005");
  server.register_handler<delay_response2>();
  server.register_handler<delay_response3>();
  server.async_start();
  rpc_client client;
  sync_wait(client.get_executor(), client.connect("127.0.0.1:9005"));
  auto result1 = sync_wait(
      client.get_executor(),
      client.call_for<delay_response2>(std::chrono::minutes(2), "test"));
  CHECK(result1.value == "test");
  result1 =
      sync_wait(client.get_executor(), client.call<delay_response2>("test"));
  CHECK(result1.value == "test");
  auto result2 = sync_wait(
      client.get_executor(),
      client.call_for<delay_response3>(std::chrono::minutes(2), "test"));
  CHECK(result2.value == "test");
  server.stop();
}

// TODO: client pool
asio::awaitable<void> test_router() {
  rpc_router router;
  router.register_handler<add>();
  router.register_handler<foo>();
  router.register_handler<round1>();
  router.register_handler<echo>();

  router.register_handler<echo_coro>();
  router.register_handler<no_arg_coro>();
  router.register_handler<no_arg_coro1>();
  router.register_handler<add_coro>();
  router.register_handler<no_arg>();

  auto name = router.get_name_by_key(get_key<add>());
  CHECK(name == "add");
  router.remove_handler<add>();
  name = router.get_name_by_key(get_key<add>());
  CHECK(name != "add");
  router.register_handler<add>();
  name = router.get_name_by_key(get_key<add>());
  CHECK(name == "add");
  CHECK_THROWS_AS(router.register_handler<add>(), std::invalid_argument);

  name = router.get_name_by_key(111);
  CHECK(name == "111");

  {
    auto ret0 = co_await router.route(get_key<no_arg>(), "");
    CHECK(ret0.ec == rpc_errc::ok);

    auto ret = co_await router.route(get_key<no_arg_coro>(), "");
    CHECK(ret.ec == rpc_errc::ok);

    auto ret1 = co_await router.route(get_key<no_arg_coro1>(), "");
    CHECK(ret1.ec == rpc_errc::ok);

    auto ret2 = co_await router.route(get_key<echo_coro>(), "test");
    CHECK(ret2.ec == rpc_errc::ok);

    router.register_handler<exception_func>();
    router.register_handler<unknown_exception_func>();
    auto ret3 = co_await router.route(get_key<exception_func>(), "");
    CHECK(ret3.ec == rpc_errc::function_exception);
    auto ret4 = co_await router.route(get_key<unknown_exception_func>(), "");
    CHECK(ret4.ec == rpc_errc::function_unknown_exception);
    auto ret5 = co_await router.route(111, "");
    CHECK(ret5.ec == rpc_errc::no_such_function);
  }

  dummy d{};
  router.register_handler<&dummy::add>(&d);
  router.register_handler<&dummy::foo>(&d);
  router.register_handler<&dummy::round1>(&d);
  router.register_handler<&dummy::echo>(&d);
  router.register_handler<&dummy::no_arg_coro>(&d);
  router.register_handler<&dummy::no_arg_coro1>(&d);
  router.register_handler<&dummy::echo_coro>(&d);
  router.register_handler<&dummy::add_coro>(&d);
  {
    auto ret = co_await router.route(get_key<&dummy::no_arg_coro>(), "");
    CHECK(ret.ec == rpc_errc::ok);
    auto ret1 = co_await router.route(get_key<&dummy::no_arg_coro1>(), "");
    CHECK(ret1.ec == rpc_errc::ok);
    auto ret2 = co_await router.route(get_key<&dummy::echo_coro>(), "test");
    CHECK(ret2.ec == rpc_errc::ok);
  }

  {
    auto s = rpc_codec::pack_args(1);
    auto s1 = rpc_codec::pack_args("test");
    auto r = co_await router.route(get_key<round1>(), s);
    auto r1 = co_await router.route(get_key<echo>(), s1);

    auto r2 = co_await router.route(get_key<&dummy::round1>(), s);
    auto r3 = co_await router.route(get_key<&dummy::echo>(), s1);
    std::cout << "\n";
  }

  auto args = rpc_codec::pack_args(1, 2);
  std::string_view str(args.data(), args.size());

  {
    auto r1 = co_await router.route(get_key<add_coro>(), str);
    auto r2 = co_await router.route(get_key<&dummy::add_coro>(), str);
    std::cout << "\n";
  }

  auto args1 = rpc_codec::pack_args("it is a test");
  std::string_view str1(args1.data(), args1.size());

  {
    auto result = co_await router.route(get_key<&dummy::add>(), str);
    auto r = rpc_codec::unpack<int>(result.result);
    auto result1 = co_await router.route(get_key<&dummy::foo>(), str1);
    CHECK(r == 3);
    CHECK(result1.ec == rpc_errc::ok);
  }

  auto result = co_await router.route(get_key<add>(), str);
  CHECK(result.ec == rpc_errc::ok);

  auto result1 = co_await router.route(get_key<foo>(), str1);
  CHECK(result1.ec == rpc_errc::ok);
}

TEST_CASE("test router") { sync_wait(get_global_executor(), test_router()); }

TEST_CASE("test rpc_connection") {
  uint64_t conn_id = 999;
  size_t num_thread = 4;
  asio::io_context io_ctx;
  tcp_socket socket(io_ctx);
  bool cross_ending_ = false;
  rpc_router router;
  router.register_handler<get_person>();

  person p{1, "tom", 20};

  auto buf = rpc_codec::pack_args(std::tuple(p));
  auto ret = sync_wait(get_global_executor(),
                       router.route(get_key<get_person>(), buf));
  auto tp = rpc_codec::unpack<person>(ret.data());
  dummy d{};
  router.register_handler<&dummy::add>(&d);
  auto conn = std::make_shared<rpc_connection>(std::move(socket), conn_id,
                                               router, cross_ending_);
  CHECK_EQ(conn->id(), conn_id);
  conn->set_check_timeout(true);
  conn->set_last_time();
  auto last_rw_time = conn->get_last_rwtime();
  std::cout << "topic_id: " << conn->topic_id() << std::endl;
  conn->close();
  io_ctx.run();
}

TEST_CASE("test context pool") {
  io_context_pool pool(0);
  CHECK(pool.size() == 1);
}

TEST_CASE("test server start") {
  rpc_server server("127.0.0.1:9005");
  server.register_handler<add>();
  server.register_handler<foo>();
  server.register_handler<no_arg>();
  server.register_handler<echo>();
  server.register_handler<echo_sv>();
  server.register_handler<round1>();
  server.register_handler<get_person>();
  server.register_handler<modify_person>();
  server.set_conn_max_age(std::chrono::seconds(5));
  server.set_check_conn_interval(std::chrono::seconds(1));
  auto ec = server.async_start();
  CHECK(!ec);
  rpc_client cl;

  static_assert(util::CharArrayRef<char const(&)[5]>);
  static_assert(util::CharArray<const char[5]>);
  rpc_codec::pack_args();
  rpc_codec::pack_args(1, 2);
  auto s1 = rpc_codec::pack_args("test");
  auto s2 = rpc_codec::pack_args(std::string_view("test2"));
  auto s3 = rpc_codec::pack_args(std::string("test2"));
  auto s5 = rpc_codec::pack_args(123);

  auto conn_ec = sync_wait(cl.get_executor(), cl.connect("127.0.0.1:9005"));
  CHECK(!conn_ec);
  
  {
    auto result = sync_wait(cl.get_executor(),
                            cl.call_for<add>(std::chrono::minutes(2), 1, 2));
    CHECK(result.ec == rpc_errc::ok);
    CHECK(result.value == 3);
  }
  {
    person p{1, "tom", 20};
    auto result = sync_wait(
        cl.get_executor(), cl.call_for<get_person>(std::chrono::minutes(2), p));
    CHECK(result.ec == rpc_errc::ok);

    auto result1 = sync_wait(
        cl.get_executor(), cl.call_for<modify_person>(std::chrono::minutes(2),
                                                      p, std::string("jack")));
    CHECK(result1.ec == rpc_errc::ok);
    CHECK(result1.value.name == "jack");
  }
  {
    auto result =
        sync_wait(cl.get_executor(),
                  cl.call_for<echo_sv>(std::chrono::minutes(2), "test"));
    CHECK(result.ec == rpc_errc::ok);
    auto result1 = sync_wait(
        cl.get_executor(), cl.call_for<echo>(std::chrono::minutes(2), "test"));
    CHECK(result1.ec == rpc_errc::ok);
    cl.enable_tcp_no_delay(false);
    auto result2 = sync_wait(cl.get_executor(), cl.call<round1>(1));
    CHECK(result2.ec == rpc_errc::ok);
  }

  {
    auto result =
        sync_wait(cl.get_executor(),
                  cl.call_for<add>(std::chrono::milliseconds(0), 1, 2));
    CHECK(result.ec == rpc_errc::request_timeout);
  }

  ec = server.async_start();
  CHECK(!ec);
  ec = server.async_start();
  CHECK(!ec);
  server.stop();
  server.stop();

  {
    auto result = sync_wait(cl.get_executor(), cl.call<add>(1, 2));
    CHECK(result.ec != rpc_errc::ok);
  }

  ec = server.async_start();
  CHECK(ec);
  std::cout << ec.message() << "\n";

  rpc_server server1("127.0.0.1:9007");
  ec = server1.async_start();
  CHECK(!ec);

  rpc_server server2("127.0.0.1:9007");
  ec = server2.async_start();
  CHECK(ec);
  std::cout << ec.message() << "\n";
  ec = server2.async_start();
  CHECK(ec);
  std::cout << ec.message() << "\n";

  // if server has stopped, start server will be failed.
  rpc_server server3("127.0.0.1:9005");
  server3.stop();
  CHECK(server3.has_stopped());
  ec = server3.async_start();
  CHECK(ec);
  std::cout << ec.message() << "\n";
}

TEST_CASE("test cross ending") {
  rpc_server server("127.0.0.1:9004");
  server.register_handler<echo>();
  server.enable_cross_ending(true);
  server.async_start();

  rpc_client client{};
  client.enable_cross_ending(true);
  sync_wait(get_global_executor(), client.connect("127.0.0.1:9004"));
  auto ret = sync_wait(client.get_executor(), client.call<echo>("test"));
  CHECK(ret.value == "test");
}

TEST_CASE("test pub sub") {
  rpc_server server("127.0.0.1:9004");
  server.async_start();

  rpc_client client{};
  sync_wait(get_global_executor(), client.connect("127.0.0.1:9004"));

  std::promise<void> promise;
  auto sub = [&]() -> asio::awaitable<void> {
    for (int i = 0; i < 5; i++) {
      auto result = co_await client.subscribe<std::string>("topic1");
      REST_LOG_INFO << result.value;
      CHECK(result.ec == rpc_errc::ok);
      CHECK(result.value == "publish message");
    }

    promise.set_value();
  };

  asio::co_spawn(client.get_executor(), sub(), asio::detached);
  std::this_thread::sleep_for(std::chrono::seconds(2));

  auto pub = [&]() -> asio::awaitable<void> {
    for (int i = 0; i < 5; i++) {
      co_await server.publish("topic1", "publish message");
    }
  };

  asio::co_spawn(client.get_executor(), pub(), asio::detached);

  promise.get_future().wait();
}

TEST_CASE("test reconnect") {
  rpc_server server("127.0.0.1:9004");
  server.async_start();
  rpc_server server1("127.0.0.1:9005");
  server1.set_check_conn_interval(std::chrono::milliseconds(100));
  server1.set_conn_max_age(std::chrono::milliseconds(400));
  server1.async_start();

  rpc_client client{};
  auto ec = sync_wait(get_global_executor(), client.connect("127.0.0.1:9004"));
  CHECK(!ec);
  ec = sync_wait(get_global_executor(), client.connect("127.0.0.1:9005"));
  CHECK(!ec);

  CHECK(server1.connection_count() == 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  // expired connection has been removed.
  CHECK(server1.connection_count() == 0);

  ec = sync_wait(get_global_executor(), client.connect("127.0.0.1:9999"));
  CHECK(ec);
  REST_LOG_INFO << ec.message();
  ec = sync_wait(
      get_global_executor(),
      client.connect("127.0.0.0:9006", std::chrono::milliseconds(200)));
  CHECK(ec);
  REST_LOG_INFO << ec.message();
  ec =
      sync_wait(get_global_executor(),
                client.connect("127.0.0.x:9006", std::chrono::milliseconds(0)));
  CHECK(ec);
  REST_LOG_INFO << ec.message();
  ec = sync_wait(
      get_global_executor(),
      client.connect("127.0.0.x:9006", std::chrono::milliseconds(200)));
  CHECK(ec);
  REST_LOG_INFO << ec.message();
}

TEST_CASE("test server address") {
  rpc_server server("127.0.0.1", "9004");
  server.enable_tcp_no_delay(false);
  auto ec = server.async_start();
  CHECK(!ec);

  server.register_handler<add>();
  server.remove_handler("add");
  CHECK_NOTHROW(server.register_handler<add>());
  server.remove_handler<add>();
  CHECK_NOTHROW(server.register_handler<add>());

  rpc_server server1("127.0.0.x:9004");
  ec = server1.async_start();
  CHECK(ec);
  REST_LOG_INFO << ec.message();

  rpc_server server2("127.0.0.1:9005");
  std::thread thd([&] { server2.start(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  std::thread thd2([&] { server2.stop(); });
  thd2.join();
  thd.join();
}

// bool in_user_pack = false;
// bool in_user_unpack = false;
// namespace user_codec {
// // adl lookup in user_codec namespace
// template <typename... Args>
// std::string serialize(rest_adl_tag, Args &&...args) {
//   in_user_pack = true;
//   msgpack::sbuffer buffer(2 * 1024);
//   if constexpr (sizeof...(Args) > 1) {
//     msgpack::pack(buffer, std::forward_as_tuple(std::forward<Args>(args)...));
//   } else {
//     msgpack::pack(buffer, std::forward<Args>(args)...);
//   }

//   return std::string(buffer.data(), buffer.size());
// }

// template <typename T> T deserialize(rest_adl_tag, std::string_view data) {
//   try {
//     in_user_unpack = true;
//     static msgpack::unpacked msg;
//     msgpack::unpack(msg, data.data(), data.size());
//     return msg.get().as<T>();
//   } catch (...) {
//     return T{};
//   }
// }
// } // namespace user_codec

// TEST_CASE("test user codec") {
//   auto buf = rpc_codec::pack_args(
//       std::make_tuple<int, std::string, int>(1, "tom", 20));
//   CHECK(in_user_pack);

//   std::string_view str(buf.data(), buf.size());
//   rpc_codec::unpack<std::tuple<int, std::string, int>>(str);
//   CHECK(in_user_unpack);
// }

// doctest comments
// 'function' : must be 'attribute' - see issue #182
DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(4007)
int main(int argc, char **argv) { return doctest::Context(argc, argv).run(); }
DOCTEST_MSVC_SUPPRESS_WARNING_POP
