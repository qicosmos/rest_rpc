#include <iostream>
#include <rest_rpc/client.hpp>
#include <rest_rpc/rest_rpc_server.hpp>

using namespace rest_rpc;

std::string address = "127.0.0.1:9004";

std::atomic<size_t> g_qps = 0;

std::string_view echo_sv(std::string_view str) {
  g_qps.fetch_add(1, std::memory_order::release);
  return str;
}

asio::awaitable<void> bench(std::shared_ptr<client> cl) {
  co_await cl->connect(address);
  std::string_view str = "it is a test";
  while (true) {
    auto result = co_await cl->call<echo_sv>(str);
    // std::cout <<result.value << "\n";
  }
}

void watch() {
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto qps = g_qps.exchange(0, std::memory_order::acquire);
    std::cout << "qps: " << qps << "\n";
  }
}

int main(int argc, char **argv) {
  rest_rpc_server server(address);
  server.register_handler<echo_sv>();
  if (argc == 1) {
    std::thread thd([] { watch(); });
    server.start();
    thd.join();
    return 0;
  }

  auto arg = argv[1];
  int par = atoi(arg);

  std::vector<std::shared_ptr<client>> clients;
  for (int i = 0; i < par; i++) {
    clients.push_back(std::make_shared<client>());
  }

  for (size_t i = 0; i < par; i++) {
    asio::co_spawn(clients[i]->get_executor(), bench(clients[i]),
                   asio::detached);
  }
  std::getchar();
}