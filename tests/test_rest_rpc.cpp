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

TEST_CASE("test_sync_add") {
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


TEST_CASE("test_client_reconnect_and_heartbeat") {

  rpc_client client;
  client.enable_auto_reconnect(); // automatic reconnect
  client.enable_auto_heartbeat(); // automatic heartbeat
  bool r = client.connect("127.0.0.1", 9000);
  bool flag = false;
  int cnt = 0;
  rpc_server server(9000, std::thread::hardware_concurrency());
  server.register_handler("get_person", get_person);
  server.async_run();
  while (true) {
    if (client.has_connected()) {
      flag = true;
      break;
    }
    else {
      // std::cout << "connected fail\n";
      cnt++;
    }
    if (cnt == 200) {
      flag = false;
      break;
    }
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  CHECK(flag);
}