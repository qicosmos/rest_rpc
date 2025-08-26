#include "../include/rest_rpc/io_context_pool.hpp"
#include "doctest/doctest.h"
#include <iostream>

using namespace rest_rpc;

TEST_CASE("test context pool") {
  io_context_pool pool(4);
  bool quit = false;
  std::thread thd([&pool, &quit] {
    pool.run();
    quit = true;
  });

  pool.stop();
  thd.join();
  CHECK(quit);
}

TEST_CASE("test context pool stop before run") {
  io_context_pool pool(2);
  bool quit = false;
  pool.stop();
  std::thread thd([&pool, &quit] {
    pool.run();
    quit = true;
  });

  pool.stop();
  thd.join();
  CHECK(quit);
}

TEST_CASE("test context pool multiple run") {
  io_context_pool pool(2);
  bool quit = false;
  std::thread thd([&pool, &quit] {
    pool.run();
    quit = true;
  });

  std::thread thd1([&pool] { pool.run(); });

  pool.stop();
  thd.join();
  thd1.join();
  CHECK(quit);
}

TEST_CASE("test context pool automatic stop") {
  auto pool = std::make_shared<io_context_pool>(2);
  std::promise<void> p;
  std::thread thd([&pool, &p] {
    bool block = true;
    p.set_value();
    pool->run();
    block = false;
    CHECK(block);
  });
  p.get_future().wait();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  thd.detach();
}

TEST_CASE("test get io context") {
  io_context_pool pool(2);
  CHECK(pool.size() == 2);
  auto &ctx1 = pool.get_io_context();
  auto &ctx2 = pool.get_io_context();
  auto &ctx3 = pool.get_io_context();
  auto ctx4 = pool.get_io_context();
  CHECK(ctx1 != ctx2);
  CHECK(ctx1 == ctx3);
  CHECK(ctx2 == ctx4);
}