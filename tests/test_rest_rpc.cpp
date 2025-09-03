#include "doctest/doctest.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <rest_rpc.hpp>
#include <thread>

using namespace rest_rpc;
using namespace rpc_service;

struct dummy {
  int add(rpc_conn conn, int a, int b) { return a + b; }
};

std::string echo(rpc_conn conn, const std::string &src) { return src; }
std::string echo_empty(rpc_conn conn, const std::string &src) { return ""; }

struct person {
  int id;
  std::string name;
  int age;

  MSGPACK_DEFINE(id, name, age);
};
person get_person(rpc_conn conn) { return {1, "tom", 20}; }

void server_user_data(rpc_conn conn) {
  auto shared_conn = conn.lock();
  if (shared_conn) {
    shared_conn->set_user_data(std::string("aa"));
    auto s = conn.lock()->get_user_data<std::string>();
    CHECK_EQ(s, "aa");
  }
  return;
}

void hello(rpc_conn conn, const std::string &str) {
  std::cout << "hello " << str << std::endl;
}

// if you want to response later, you can use async model, you can control when
// to response
void delay_echo(rpc_conn conn, const std::string &src) {
  auto sp = conn.lock();
  sp->set_delay(true);
  auto req_id = sp->request_id(); // note: you need keep the request id at that
  // time, and pass it into the async thread
  std::thread thd([conn, req_id, src] {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto conn_sp = conn.lock();
    if (conn_sp) {
      conn_sp->pack_and_response(req_id, std::move(src));
    }
  });
  thd.detach();
}

struct empty_obj {
  MSGPACK_DEFINE()
};

empty_obj get_empty_obj(rpc_conn conn) { return {}; }

TEST_CASE("test_client_reconnect") {
  rpc_client client;
  client.enable_auto_reconnect(); // automatic reconnect
  client.enable_auto_heartbeat(); // automatic heartbeat
  client.set_connect_timeout(1000);
  client.set_reconnect_count(10);
  client.set_error_callback([](asio::error_code ec) {
    std::cout << "line: " << __LINE__ << ", msg: " << ec.message() << std::endl;
  });
  client.connect("127.0.0.1", 9000);

  rpc_server server(9000, std::thread::hardware_concurrency());
  dummy d;
  server.register_handler("add", &dummy::add, &d);
  server.async_run();

  int count = 0;
  while (true) {
    if (client.has_connected()) {
      try {
        auto result = client.call<int>("add", 1, 2);
        CHECK_EQ(result, 3);
        break;
      } catch (const std::exception &ex) {
        std::cout << ex.what() << std::endl;
      }
    } else {
      count++;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

TEST_CASE("test_client_default_constructor") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  dummy d;
  server.register_handler("add", &dummy::add, &d);
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  rpc_client client;
  client.update_addr("127.0.0.1", 9000);
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
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

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
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

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
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  rpc_client client("127.0.0.1", 9000);
  bool r = client.connect();
  CHECK(r);
  auto result = client.call<int>("add", 1, 2);
  CHECK_EQ(result, 3);
  CHECK_THROWS_WITH_AS(client.call<int>("add", 1, "2"),
                       "unpack failed: Args not match!", std::logic_error);
  CHECK_THROWS_WITH_AS(client.call<int>("Add", 1, 2),
                       "unknown function: 3961593724", std::logic_error);
}

TEST_CASE("test_client_sync_call_return_void") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  server.register_handler("echo", echo);
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  rpc_client client("127.0.0.1", 9000);
  bool r = client.connect();
  CHECK(r);
  client.call<>("echo");
}

TEST_CASE("test_client_async_call_empty_obj") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  server.register_handler("get_empty_obj", get_empty_obj);
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  rpc_client client("127.0.0.1", 9000);
  bool r = client.connect();
  CHECK(r);

  client.set_error_callback(
      [](asio::error_code ec) { std::cout << ec.message() << std::endl; });

  auto f = client.async_call<FUTURE>("get_empty_obj");
  if (f.wait_for(std::chrono::milliseconds(50)) ==
      std::future_status::timeout) {
    std::cout << "timeout" << std::endl;
  } else {
    auto p = f.get().as<empty_obj>();
  }
}

TEST_CASE("test_client_async_call") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  server.register_handler("get_person", get_person);
  server.register_handler("hello", hello);
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

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
  CHECK_NOTHROW(fu.get().as()); // no return
  auto fu_err = client.async_call<FUTURE>("Hello", "purecpp");
  CHECK_THROWS_WITH_AS(fu_err.get().as(), "unknown function: 2333776211",
                       std::logic_error);
}
TEST_CASE("test_client_async_call_not_connect") {
  rpc_client client("127.0.0.1", 9001);
  client.async_call<person>("get_person",
                            [](const asio::error_code &ec, person data) {
                              CHECK_EQ(ec, asio::error::not_connected);
                            });
}

TEST_CASE("test_client_async_call_with_timeout") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  server.register_handler("echo", echo);
  server.register_handler("get_person", get_person);
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  rpc_client client;
  bool r = client.connect("127.0.0.1", 9000);
  CHECK(r);
  std::string test = "test async call with timeout";
  // zero means no timeout check, no param means using default timeout(5s)
  client.async_call<std::string, 0>(
      "echo",
      [](const asio::error_code &ec, std::string data) {
        if (ec)
          std::cout << "error code: " << ec << ", err msg: " << data << '\n';
      },
      test);
  client.async_call<std::string>(
      "echo",
      [&client](const asio::error_code &ec, std::string data) {
        std::cout << "req id : " << client.reqest_id() << '\n';
        if (ec)
          std::cout << "error code: " << ec << ", err msg: " << data << '\n';
      },
      test);
  client.async_call<person>(
      "get_person",
      [&client](const asio::error_code &ec, person p) {
        if (ec) {
          std::cout << "error code: " << ec << ", err msg: " << ec.message()
                    << '\n';
          return;
        }
        CHECK_EQ(p.id, 1);
        CHECK_EQ(p.age, 20);
        CHECK_EQ(p.name, "tom");
      },
      test);
  auto f = client.async_call<FUTURE>("get_person");
  if (f.wait_for(std::chrono::milliseconds(500)) ==
      std::future_status::timeout) {
    std::cout << "timeout" << std::endl;
  } else {
    auto p = f.get().as<person>();
    CHECK_EQ(p.id, 1);
    CHECK_EQ(p.age, 20);
    CHECK_EQ(p.name, "tom");
  }
}

TEST_CASE("test_client_subscribe") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  server.register_handler<true>(
      "publish", [&server](rpc_conn conn, std::string key, std::string token,
                           std::string val) {
        CHECK(val == "hello subscriber");
        server.publish(std::move(key), std::move(val));
      });
  bool stop = false;
  std::thread thd([&server, &stop] {
    while (!stop) {
      server.publish("key", "hello subscriber");
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  rpc_client client;
  bool r = client.connect("127.0.0.1", 9000);
  CHECK(r);
  client.publish("key", "hello subscriber");

  client.subscribe("key", [&stop](string_view data) {
    std::cout << data << "\n";
    CHECK_EQ(data, "hello subscriber");
    stop = true;
  });
  thd.join();
}

TEST_CASE("test_client_subscribe_not_exist_key") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  server.register_handler("publish",
                          [&server](rpc_conn conn, std::string key,
                                    std::string token, std::string val) {
                            server.publish(std::move(key), std::move(val));
                          });
  bool stop = false;
  server.set_error_callback([&stop](asio::error_code ec, string_view msg) {
    std::cout << "line: " << __LINE__ << ", msg: " << ec.message() << " -- "
              << msg << std::endl;
    CHECK_EQ(ec, asio::error::invalid_argument);
    stop = true;
  });
  server.set_conn_timeout_callback([](int64_t conn_id) {
    std::cout << "connect id : " << conn_id << " timeout \n";
  });
  std::thread thd([&server, &stop] {
    while (!stop) {
      server.publish("key", "hello subscriber");
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  });
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  rpc_client client;
  bool r = client.connect("127.0.0.1", 9000);
  CHECK(r);
  client.set_error_callback([&stop](asio::error_code ec) {
    std::cout << "line: " << __LINE__ << ", msg: " << ec.value() << " -- "
              << ec.message() << std::endl;
  });
  client.subscribe("key1", [&stop](string_view data) {
    CHECK(data != "hello subscriber");
    stop = true;
  });
  thd.join();
}

TEST_CASE("test_server_publish_encode_msg") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  server.register_handler("publish",
                          [&server](rpc_conn conn, std::string key,
                                    std::string token, std::string val) {
                            server.publish(std::move(key), std::move(val));
                          });
  bool stop = false;
  std::thread thd([&server, &stop] {
    person pp{1, "tom", 20};
    while (!stop) {
      server.publish("person", pp);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  rpc_client client;
  bool r = client.connect("127.0.0.1", 9000);
  CHECK(r);
  client.subscribe("person", [&stop](string_view data) {
    try {
      msgpack_codec codec;
      person p = codec.unpack<person>(data.data(), data.size());
      CHECK_EQ(p.id, 1);
      CHECK_EQ(p.age, 20);
      CHECK_EQ(p.name, "tom");
      stop = true;
    } catch (const std::exception &ex) {
      std::cout << ex.what() << std::endl;
    }
  });
  thd.join();
}

TEST_CASE("test_client_subscribe_by_token") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  bool stop = false;
  std::thread thd([&server, &stop] {
    while (!stop) {
      auto list = server.get_token_list();
      for (auto &token : list) {
        server.publish_by_token("key", token, "hello token subscriber");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  rpc_client client;
  bool r = client.connect("127.0.0.1", 9000);
  CHECK(r);
  client.subscribe(
      "key", "048a796c8a3c6a6b7bd1223bf2c8cee05232e927b521984ba417cb2fca6df9d1",
      [&stop](string_view data) {
        std::cout << data << "\n";
        CHECK_EQ(data, "hello token subscriber");
        stop = true;
      });
  thd.join();
}

TEST_CASE("test_client_publish_and_subscribe_by_token") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  server.register_handler("publish_by_token", [&server](rpc_conn conn,
                                                        std::string key,
                                                        std::string token,
                                                        std::string val) {
    server.publish_by_token(std::move(key), std::move(token), std::move(val));
  });
  bool stop = false;
  std::thread thd([&server, &stop] {
    while (!stop) {
      auto list = server.get_token_list();
      for (auto &token : list) {
        server.publish_by_token("key", token, "hello token subscriber");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::string client_token =
      "048a796c8a3c6a6b7bd1223bf2c8cee05232e927b521984ba417cb2fca6df9d1";
  rpc_client client;
  bool r = client.connect("127.0.0.1", 9000);
  CHECK(r);
  client.publish_by_token("key", client_token, "hello token subscriber");
  client.subscribe("key", client_token, [&stop](string_view data) {
    std::cout << data << "\n";
    CHECK_EQ(data, "hello token subscriber");
    stop = true;
  });
  thd.join();
}

TEST_CASE("test_server_callback") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  dummy d;
  server.register_handler("add", &dummy::add, &d);
  server.set_network_err_callback(
      [](std::shared_ptr<connection> conn, std::string reason) {
        std::cout << "remote client address: " << conn->remote_address()
                  << " networking error, reason: " << reason << "\n";
      });
  server.set_conn_timeout_callback([](int64_t conn_id) {
    std::cout << "connect id : " << conn_id << " timeout \n";
  });
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  rpc_client client("127.0.0.1", 9000);
  bool r = client.connect();
  CHECK(r);
  auto result = client.call<int>("add", 1, 2);
  CHECK_EQ(result, 3);
}
TEST_CASE("test_server_user_data") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  server.register_handler("server_user_data", server_user_data);
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  rpc_client client("127.0.0.1", 9000);
  bool r = client.connect();
  CHECK(r);
  client.call<>("server_user_data");
}
TEST_CASE("test_server_delay_response") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  server.register_handler("delay_echo", delay_echo);
  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  rpc_client client("127.0.0.1", 9000);
  bool r = client.connect();
  CHECK(r);
  auto result = client.call<std::string>("delay_echo", "test_delay_echo");
  CHECK_EQ(result, "test_delay_echo");
}
TEST_CASE("test_server_duplicate_registration_key") {
  rpc_server server(9000, std::thread::hardware_concurrency());
  server.register_handler("delay_echo", delay_echo);
  try {
    server.register_handler("delay_echo", delay_echo);
  } catch (const std::exception &e) {
    string_view ew{e.what()};
    std::cerr << ew << '\n';
    CHECK_EQ(ew, "duplicate registration key !");
  }

  server.async_run();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  rpc_client client("127.0.0.1", 9000);
  bool r = client.connect();
  CHECK(r);
  auto result = client.call<std::string>("delay_echo", "test_delay_echo");
  CHECK_EQ(result, "test_delay_echo");
}