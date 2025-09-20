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
};

int add(int a, int b) { return a + b; }

void foo(std::string str) { std::cout << str << "\n"; }

TEST_CASE("test router") {
  rpc_router router;
  router.register_handler<add>();
  router.register_handler<foo>();

  dummy d{};
  router.register_handler<&dummy::add>(&d);
  router.register_handler<&dummy::foo>(&d);
  rpc_service::msgpack_codec codec;
  auto args = codec.pack_args(1, 2);
  std::string_view str(args.data(), args.size());

  auto args1 = codec.pack_args("it is a test");
  std::string_view str1(args1.data(), args1.size());

  {
    auto result = router.route(get_key<&dummy::add>(), str);
    auto r = codec.unpack<int>(result.result.data(), result.result.size());
    auto result1 = router.route(get_key<&dummy::foo>(), str1);
    CHECK(r == 3);
    CHECK(result1.ec == rpc_errc::ok);
  }

  auto result = router.route(get_key<add>(), str);
  CHECK(result.ec == rpc_errc::ok);

  auto result1 = router.route(get_key<foo>(), str1);
  CHECK(result1.ec == rpc_errc::ok);
}

asio::awaitable<void> void_returning_coroutine() { co_return; }

TEST_CASE("test server start") {
  rest_rpc_server server("127.0.0.1:9005");
  server.register_handler<add>();
  server.register_handler<foo>();
  auto ec = server.async_start();
  CHECK(!ec);
  client cl;
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

  auto result = sync_wait(cl.get_executor(), cl.call<add>(1, 2));

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