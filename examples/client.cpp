#include <rest_rpc.hpp>
using namespace rest_rpc;

// Only need a declaration.
std::string_view echo(std::string_view str);
int add(int a, int b);

void basic_usage() {
  auto rpc_call = []() -> asio::awaitable<void> {
    rpc_client client;
    auto ec0 = co_await client.connect("127.0.0.1:9004");
    if(ec0) {
      REST_LOG_ERROR << ec0.message();
      co_return;
    }
    
    auto r = co_await client.call<echo>("test");
    if(r.ec==rpc_errc::ok) {
      REST_LOG_INFO << "call result: " << r.value;
      assert(r.value == "test");
    }
    
    auto r1 = co_await client.call<add>(1, 2);
    if(r1.ec==rpc_errc::ok) {
      REST_LOG_INFO << "call result: " << r1.value;
      assert(r1.value == 3);
    }
  };
  
  sync_wait(get_global_executor(), rpc_call());
}

void subscribe() {
  REST_LOG_INFO << "will subscribe, waiting for publish";
  auto sub = [&]() -> asio::awaitable<void> {
    rpc_client client;
    co_await client.connect("127.0.0.1:9004");
    while (true) {
      auto [ec, result] = co_await client.subscribe<std::string>("topic1");
      if (ec != rpc_errc::ok) {
        REST_LOG_ERROR << "subscribe failed: " << make_error_code(ec).message();
        break;
      }
      
      if (result == "close") {
        co_return;
      }
      
      REST_LOG_INFO << result;
    }
  };
  
  sync_wait(get_global_executor(), sub());
}

int main() {
  basic_usage();
  subscribe();
}