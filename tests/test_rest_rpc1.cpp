#define DOCTEST_CONFIG_IMPLEMENT

#include "doctest/doctest.h"
#include <asio/any_completion_handler.hpp>
#include <rest_rpc/client.hpp>
#include <rest_rpc/rest_rpc_server.hpp>
#include <rest_rpc/traits.h>

using namespace rest_rpc;

struct dummy {
  int add(int a, int b) { return a + b; }

  void foo(std::string str) { std::cout << str << "\n"; }

  std::string echo(std::string val) { return val; }

  int round1(int i) { return i; }
};

int add(int a, int b) { return a + b; }

void foo(std::string str) { std::cout << str << "\n"; }

int round1(int i) { return i; }

std::string_view echo_sv(std::string_view str) { return str; }

std::string_view delay_response(std::string_view str) {
  auto &ctx = rpc_context::context();
  // set_delay before response in another thread
  ctx.set_delay(true);

  std::thread thd([ctx=std::move(ctx)]() mutable {
    std::this_thread::sleep_for(std::chrono::seconds(2));
//    auto ec = ctx.sync_response("it is from a detached thread");//TODO: add a safe version
//    if (ec) {
//      REST_LOG_ERROR << "response error: " << ec.message();
//    }
    auto executor = ctx.get_executor();
    auto coro = [ctx = std::move(ctx)]() mutable ->asio::awaitable<void> {
      auto ec = co_await ctx.response<delay_response>("test");//TODO: avoid duplicate response
      if (ec) {
        REST_LOG_ERROR << "response error: " << ec.message();
      }
    };
    
    async_start(executor, std::move(coro));
//    asio::co_spawn(executor, std::move(coro), asio::detached);
  });
  thd.detach();

  // this return value is meaningless, because it will response later, the
  // return type is important for client, so just return an empty value here.
  return "";
}

std::string echo(std::string str) { return str; }

asio::awaitable<std::string> echo_coro(std::string str) { co_return str; }

void no_arg() { std::cout << "no args\n"; }

TEST_CASE("test router") {
  rpc_router router;
  router.register_handler<add>();
  router.register_handler<foo>();
  router.register_handler<round1>();
  router.register_handler<echo>();
  
//  router.register_handler<echo_coro>();

  dummy d{};
  router.register_handler<&dummy::add>(&d);
  router.register_handler<&dummy::foo>(&d);
  router.register_handler<&dummy::round1>(&d);
  router.register_handler<&dummy::echo>(&d);
  rpc_service::msgpack_codec codec;
  {
    auto s = codec.pack_args(1);
    auto s1 = codec.pack_args("test");
    auto r = router.route(get_key<round1>(), s);
    auto r1 = router.route(get_key<echo>(), s1);

    auto r2 = router.route(get_key<&dummy::round1>(), s);
    auto r3 = router.route(get_key<&dummy::echo>(), s1);
    std::cout << "\n";
  }

  auto args = codec.pack_args(1, 2);
  std::string_view str(args.data(), args.size());

  auto args1 = codec.pack_args("it is a test");
  std::string_view str1(args1.data(), args1.size());

  {
    auto result = router.route(get_key<&dummy::add>(), str);
    auto r = codec.unpack<int>(result.result);
    auto result1 = router.route(get_key<&dummy::foo>(), str1);
    CHECK(r == 3);
    CHECK(result1.ec == rpc_errc::ok);
  }

  auto result = router.route(get_key<add>(), str);
  CHECK(result.ec == rpc_errc::ok);

  auto result1 = router.route(get_key<foo>(), str1);
  CHECK(result1.ec == rpc_errc::ok);
}

asio::awaitable<void> void_returning_coroutine() {
  auto executor = co_await asio::this_coro::executor;
  asio::io_context &ioc = static_cast<asio::io_context &>(executor.context());

  co_return;
}

TEST_CASE("test server start") {
  rest_rpc_server server("127.0.0.1:9005");
  server.register_handler<add>();
  server.register_handler<foo>();
  server.register_handler<no_arg>();
  server.register_handler<echo>();
  server.register_handler<echo_sv>();
  server.register_handler<round1>();
  server.register_handler<delay_response>();
  auto ec = server.async_start();
  CHECK(!ec);
  client cl;

  static_assert(util::CharArrayRef<char const(&)[5]>);
  static_assert(util::CharArray<const char[5]>);
  rpc_service::msgpack_codec::pack_args();
  rpc_service::msgpack_codec::pack_args(1, 2);
  auto s1 = rpc_service::msgpack_codec::pack_args("test");
  auto s2 = rpc_service::msgpack_codec::pack_args(std::string_view("test2"));
  auto s3 = rpc_service::msgpack_codec::pack_args(std::string("test2"));
  auto s5 = rpc_service::msgpack_codec::pack_args(123);

  //  auto future = asio::co_spawn(cl.get_executor(),
  //  cl.connect("127.0.0.1:9005"), asio::use_future); auto conn_ec =
  //  future.get();
  auto conn_ec = sync_wait(cl.get_executor(), cl.connect("127.0.0.1:9005"));
  //  async_start(cl.get_executor(), cl.connect("127.0.0.1:9005"), [](auto
  //  exptr, auto r){}); async_start(cl.get_executor(),
  //  cl.connect("127.0.0.1:9005")); auto future =
  //  async_future(cl.get_executor(), cl.connect("127.0.0.1:9005")); auto ec1 =
  //  future.get();
  //
  //  auto future1 = async_future(cl.get_executor(),
  //  void_returning_coroutine()); future1.get();

  //  auto future1 = asio::co_spawn(cl.get_executor(), cl.call<add>(1, 2),
  //  asio::use_future); auto result = future1.get();
  {
    auto result = sync_wait(cl.get_executor(), cl.call_for<add>(std::chrono::minutes(2), 1, 2));
    CHECK(result.ec == rpc_errc::ok);
  }
  {
    // auto result = sync_wait(cl.get_executor(),
    // cl.call_for<no_arg>(std::chrono::minutes(2)));
    // CHECK(result.ec==rpc_errc::ok);
    auto result0 =
        sync_wait(cl.get_executor(),
                  cl.call_for<delay_response>(std::chrono::minutes(2), "test"));
    CHECK(result0.ec == rpc_errc::ok);

    auto result =
        sync_wait(cl.get_executor(),
                  cl.call_for<echo_sv>(std::chrono::minutes(2), "test"));
    CHECK(result.ec == rpc_errc::ok);
    auto result1 = sync_wait(
        cl.get_executor(), cl.call_for<echo>(std::chrono::minutes(2), "test"));
    CHECK(result1.ec == rpc_errc::ok);
    auto result2 = sync_wait(cl.get_executor(), cl.call<round1>(1));
    CHECK(result2.ec == rpc_errc::ok);
  }
  

  ec = server.async_start();
  CHECK(!ec);
  ec = server.async_start();
  CHECK(!ec);
  server.stop();
  server.stop();

  ec = server.async_start();
  CHECK(ec);
  std::cout << ec.message() << "\n";

  rest_rpc_server server1("127.0.0.1:9007");
  ec = server1.async_start();
  CHECK(!ec);

  rest_rpc_server server2("127.0.0.1:9007");
  ec = server2.async_start();
  CHECK(ec);
  std::cout << ec.message() << "\n";
  ec = server2.async_start();
  CHECK(ec);
  std::cout << ec.message() << "\n";

  // if server has stopped, start server will be failed.
  rest_rpc_server server3("127.0.0.1:9005");
  server3.stop();
  CHECK(server3.has_stopped());
  ec = server3.start();
  CHECK(ec);
  std::cout << ec.message() << "\n";
}

// doctest comments
// 'function' : must be 'attribute' - see issue #182
DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(4007)
int main(int argc, char **argv) { return doctest::Context(argc, argv).run(); }
DOCTEST_MSVC_SUPPRESS_WARNING_POP