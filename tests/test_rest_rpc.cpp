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
    // auto shared_conn = conn.lock();
    // if (shared_conn) {
    //   shared_conn->set_user_data(std::string("aa"));
    //   auto s = conn.lock()->get_user_data<std::string>();
    //   std::cout << s << '\n'; // aa
    // }
    return a + b;
  }
};

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