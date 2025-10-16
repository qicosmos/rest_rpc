#include <rest_rpc.hpp>
using namespace rest_rpc;

std::string_view echo(std::string_view str) {
  return str;
}

struct dummy{
  int add(int a, int b) { return a + b; }
};
int add(int a, int b) { return a + b; }

struct person {
  int id;
  std::string name;
  int age;
};

person get_person(person p) {
  p.name = "jack";
  return p;
}

void basict_usage() {
  rpc_server server("127.0.0.1:9004", 4);
  server.register_handler<echo>();
  
  dummy d{};
  server.register_handler<&dummy::add>(&d);
  
  server.register_handler<get_person>();
  
  auto ec = server.async_start();
  if(ec) {
    REST_LOG_ERROR << ec.message();
    return;
  }
  
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
    
    auto r1 = co_await client.call<&dummy::add>(1, 2);
    if(r1.ec==rpc_errc::ok) {
      REST_LOG_INFO << "call result: " << r1.value;
      assert(r1.value == 3);
    }
    
    person p{1, "tom", 20};
    auto r2 = co_await client.call<get_person>(p);
    if(r2.ec==rpc_errc::ok) {
      REST_LOG_INFO << "call result: " << r2.value.name;
      assert(r2.value.name == "jack");
    }
  };
  
  sync_wait(get_global_executor(), rpc_call());
}

void publish() {
  rpc_server server("127.0.0.1:9004", 4);
  server.register_handler<echo>();
  server.register_handler<add>();
  server.async_start();
  
  REST_LOG_INFO << "will pubish, waiting for input";
  auto pub = [&]() -> asio::awaitable<void> {
    std::string str;
    while (true) {
      std::cin >> str;
      if(str == "quit") {
        break;
      }
      
      co_await server.publish("topic1", str);
    }
  };
  
  sync_wait(get_global_executor(), pub());
  server.stop();
}

int main() {
  basict_usage();
  
  publish();
}