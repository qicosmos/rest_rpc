#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <rest_rpc.hpp>
#include "doctest/doctest.h"

using namespace rest_rpc;
using namespace rpc_service;

struct dummy {
  int add(rpc_conn conn, int a, int b) {
    return a + b;
  }
};

std::string echo(rpc_conn conn, const std::string &src) { return src; }

struct person {
  int id;
  std::string name;
  int age;

  MSGPACK_DEFINE(id, name, age);
};
person get_person(rpc_conn conn) { return {1, "tom", 20}; }

void hello(rpc_conn conn, const std::string &str) {
  std::cout << "hello " << str << std::endl;
}

TEST_CASE("test_client_default_constructor") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  dummy d;
  server.register_handler("add", &dummy::add, &d);
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  rpc_client client;
  bool r = client.connect("127.0.0.1", 9000);
  CHECK(r);
  auto result = client.call<int>("add", 1, 2);
  CHECK_EQ(result, 3);
}

TEST_CASE("test_constructor_with_language") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  dummy d;
  server.register_handler("add", &dummy::add, &d);
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  rpc_client client(client_language_t::CPP, nullptr);
  bool r = client.connect("127.0.0.1", 9000);
  CHECK(r);
  auto result = client.call<int>("add", 1, 2);
  CHECK_EQ(result, 3);
}

TEST_CASE("test_client_async_connect") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  dummy d;
  server.register_handler("add", &dummy::add, &d);
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  rpc_client client;
  client.async_connect("127.0.0.1", 9000);

  try {
    auto result = client.call<int>("add", 1, 2);
    CHECK_EQ(result, 3);
  } catch (const std::exception &ex) {
    std::cout << ex.what() << std::endl;
  }
}

TEST_CASE("test_client_sync_call") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  dummy d;
  server.register_handler("add", &dummy::add, &d);
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  rpc_client client("127.0.0.1", 9000);
  bool r = client.connect();
  CHECK(r);
  auto result = client.call<int>("add", 1, 2);
  CHECK_EQ(result, 3);
}

TEST_CASE("test_client_async_call") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  server.register_handler("get_person", get_person);
  server.register_handler("hello", hello);
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  rpc_client client("127.0.0.1", 9000);
  bool r = client.connect();
  CHECK(r);

  client.set_error_callback(
      [](asio::error_code ec) { std::cout << ec.message() << std::endl; });

  auto f = client.async_call<FUTURE>("get_person");
  if (f.wait_for(std::chrono::milliseconds(50)) ==
      std::future_status::timeout) {
    std::cout << "timeout" << std::endl;
  } else {
    auto p = f.get().as<person>();
    CHECK_EQ(p.id, 1);
    CHECK_EQ(p.age, 20);
    CHECK_EQ(p.name, "tom");
  }
  auto fu = client.async_call<FUTURE>("hello", "purecpp");
  fu.get().as(); // no return
}
TEST_CASE("test_client_async_call_not_connect") {
  rpc_client client("127.0.0.1", 9001);
  client.async_call<>("get_person",
                      [](const asio::error_code &ec, string_view data) {
                        CHECK_EQ(ec, asio::error::not_connected);
                        CHECK_EQ(data, "not connected");
                      });
}

TEST_CASE("test_client_async_call_with_timeout") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  server.register_handler("echo", echo);
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  rpc_client client;
  bool r = client.connect("127.0.0.1", 9000);
  CHECK(r);
  std::string test = "test async call with timeout";
  // zero means no timeout check, no param means using default timeout(5s)
  client.async_call<0>(
      "echo",
      [](const asio::error_code &ec, string_view data) {
        auto str = as<std::string>(data);
        std::cout << "echo " << str << '\n';
      },
      test);
}
