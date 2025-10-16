# rest_rpc

| OS (Compiler Version)                          | Status                                                                                                   |
|------------------------------------------------|----------------------------------------------------------------------------------------------------------|
| Ubuntu 22.04 (clang 14.0.0)                    | ![win](https://github.com/qicosmos/rest_rpc/actions/workflows/linux_clang.yml/badge.svg?branch=master) |
| Ubuntu 22.04 (gcc 11.2.0)                      | ![win](https://github.com/qicosmos/rest_rpc/actions/workflows/linux_gcc.yml/badge.svg?branch=master)   |
| macOS Monterey 12 (AppleClang 14.0.0.14000029) | ![win](https://github.com/qicosmos/rest_rpc/actions/workflows/mac.yml/badge.svg?branch=master)         |
| Windows Server 2022 (MSVC 19.33.31630.0)       | ![win](https://github.com/qicosmos/rest_rpc/actions/workflows/windows.yml/badge.svg?branch=master)     |

c++20, high performance, cross platform, easy to use rpc framework.

It's so easy to love RPC.

Modern C++开发的RPC库就是这么简单好用！

# rest_rpc简介

rest_rpc是一个高性能、易用、跨平台、header only的基于c++20 协程的rpc 库，它的目标是让tcp通信变得非常简单易用，即使不懂网络通信的人也可以直接使用它。它依赖header-only的standalone [asio](https://github.com/chriskohlhoff/asio)(tag:asio-1-36-0) 

可以快速上手，使用者只需要关注自己的业务逻辑即可。

# 编译器版本
需要完全支持c++20 的编译器：

gcc12+, clang15+, msvc2022

# 谁在用rest_rpc

1. 博世汽车

[在这里增加用户](https://github.com/qicosmos/rest_rpc/wiki/%E4%BD%BF%E7%94%A8rest_rpc%E7%9A%84%E7%94%A8%E6%88%B7%E5%88%97%E8%A1%A8)

# rest_rpc的特点

## 易

rest_rpc为用户提供了非常简单易用的接口，几行代码就可以实现rpc通信了，来看第一个例子

### 一个加法的rpc服务

```
//服务端注册加法rpc服务

int add(rpc_conn conn, int a, int b) { return a + b; }

int main(){
  rpc_server server("127.0.0.1:9004", std::thread::hardware_concurrency());

  server.register_handler<add>();

  server.start();
}
```

```
//客户端调用加法的rpc服务
int main(){
  auto rpc_call = []() -> asio::awaitable<void> {
    rpc_client client;
    auto ec = co_await client.connect("127.0.0.1:9004");
    if(ec) {
      REST_LOG_ERROR << ec0.message();
      co_return;
    }
    
    auto r = co_await client.call<add>(1, 2);
    if(r.ec==rpc_errc::ok) {
      REST_LOG_INFO << "call result: " << r.value;
      assert(r.value == 3);
    }
  };
  
  sync_wait(get_global_executor(), rpc_call());
}
```

### 获取一个对象的rpc服务

```
//服务端注册获取person的rpc服务

//1.先定义person对象
struct person {
  int id;
  std::string name;
  int age;
};

//2.提供并服务
person get_person(person p) {
  p.name = "jack";
  return p;
}

int main(){
  rpc_server server("127.0.0.1:9004", std::thread::hardware_concurrency());
  server.register_handler<get_person>();
  server.start();
}
```

```
//客户端调用获取person对象的rpc服务
int main(){
  auto rpc_call = []() -> asio::awaitable<void> {
    rpc_client client;
    auto ec = co_await client.connect("127.0.0.1:9004");
    if(ec) {
      REST_LOG_ERROR << ec0.message();
      co_return;
    }
    
    person p{1, "tom", 20};
    auto r = co_await client.call<get_person>(p);
    if(r.ec==rpc_errc::ok) {
      REST_LOG_INFO << "call result: " << r.value.name;
      assert(r.value.name == "jack");
    }
  };
  
  sync_wait(get_global_executor(), rpc_call());
}
```

## 发布订阅
以订阅某个topic为例：

server 端代码：
```cpp
void publish() {
  rpc_server server("127.0.0.1:9004", 4);
  server.async_start();
  
  REST_LOG_INFO << "will pubish, waiting for input";
  auto pub = [&]() -> asio::awaitable<void> {
    std::string str;
    while (true) {
      std::cin >> str;
      if(str == "quit") {
        break;
      }
      
      co_await server.publish("topic1", str);// 向客户端发布一个string，你也可以发布一个对象，内部会自动序列化
    }
  };
  
  sync_wait(get_global_executor(), pub());
}

client 端代码：
```cpp
void subscribe() {
  REST_LOG_INFO << "will subscribe, waiting for publish";
  auto sub = [&]() -> asio::awaitable<void> {
    rpc_client client;
    co_await client.connect("127.0.0.1:9004");
    while (true) {
      // 订阅topic1，会自动将结果反序列化为std::string, 如果publish是一个person对象，则subscribe参数填person，内部会自动反序列化
      auto [ec, result] = co_await client.subscribe<std::string>("topic1");
      if (ec != rpc_errc::ok) {
        REST_LOG_ERROR << "subscribe failed: " << make_error_code(ec).message();
        break;
      }
      
      REST_LOG_INFO << result;
    }
  };
  
  sync_wait(get_global_executor(), sub());
}
```

## 快

rest_rpc是目前最快的rpc库，具体和grpc和brpc做了性能对比测试，rest_rpc性能是最高的，远超grpc。

性能测试代码在这里：

https://github.com/qicosmos/rest_rpc/tree/master/tests/bench.cpp

## 使用自己的序列化库
rest_rpc 默认使用yalantinglibs的struct_pack 去做系列化/反序列化的，它的性能非常好。

rest_rpc 也支持用户使用自己的序列化库，只需要去实现一个序列化和一个反序列化函数。
```cpp
 namespace user_codec {
 // adl lookup in user_codec namespace
 template <typename... Args>
 std::string serialize(rest_adl_tag, Args &&...args) {
   msgpack::sbuffer buffer(2 * 1024);
   if constexpr (sizeof...(Args) > 1) {
     msgpack::pack(buffer, std::forward_as_tuple(std::forward<Args>(args)...));
   } else {
     msgpack::pack(buffer, std::forward<Args>(args)...);
   }

   return std::string(buffer.data(), buffer.size());
 }

 template <typename T> T deserialize(rest_adl_tag, std::string_view data) {
   try {
     static msgpack::unpacked msg;
     msgpack::unpack(msg, data.data(), data.size());
     return msg.get().as<T>();
   } catch (...) {
     return T{};
   }
 }
 } // namespace user_codec
```
实现这两个函数之后rest_rpc 将会使用自定义的序列化/反序列化函数了。

# rest_rpc的更多用法

可以参考rest_rpc的example:

https://github.com/qicosmos/rest_rpc/tree/master/examples


## 社区和群
purecpp.cn

qq群:546487929
