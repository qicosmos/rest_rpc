# rest_rpc
c++11, high performance, cross platform, easy to use rpc framework.

It's so easy to love RPC.

Modern C++开发的RPC库就是这么简单好用！

# rest_rpc简介

rest_rpc是一个高性能、易用、跨平台、header only的c++11 rpc库，它的目标是让tcp通信变得非常简单易用，即使不懂网络通信的人也可以直接使用它。
可以快速上手，使用者只需要关注自己的业务逻辑即可。

# rest_rpc的特点

## 易

rest_rpc为用户提供了非常简单易用的接口，几行代码就可以实现rpc通信了，来看第一个例子

### 一个加法的rpc服务

```
//服务端注册加法rpc服务

struct dummy{
	int add(rpc_conn conn, int a, int b) { return a + b; }
};

int main(){
	rpc_server server(9000, std::thread::hardware_concurrency());

	dummy d;
	server.register_handler("add", &dummy::add, &d);
	
	server.run();
}
```

```
//客户端调用加法的rpc服务
int main(){
	rpc_client client("127.0.0.1", 9000);
	client.connect();

	int result = client.call<int>("add", 1, 2);

	client.run();
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

	MSGPACK_DEFINE(id, name, age);
};

//2.提供并服务
person get_person(rpc_conn conn) {
	return { 1, "tom", 20 };
}

int main(){
	//...
	server.register_handler("get_person", get_person);
}
```

```
//客户端调用获取person对象的rpc服务
int main(){
	rpc_client client("127.0.0.1", 9000);
	client.connect();
	
	person result = client.call<person>("get_person");
	std::cout << result.name << std::endl;
	
	client.run();
}
```

## 酷

异步？

同步？

future?

callback？

当初为了提供什么样的接口在社区群里还争论了一番，有人希望提供callback接口，有人希望提供future接口，最后我
决定都提供，专治强迫症患者:)

现在想要的这些接口都给你提供了，你想用什么类型的接口就用什么类型的接口，够酷吧，让我们来看看怎么用这些接口吧：

```
//服务端提供echo服务
std::string echo(rpc_conn conn, const std::string& src) {
	return src;
}

server.register_handler("echo", echo);
```

客户端同步接口

```
auto result = client.call<std::string>("echo", "hello");
```

客户端异步回调接口

```
client.async_call("echo", [](boost::system::error_code ec, string_view data){
	auto str = as<std::string>(data);
	std::cout << "echo " << str << '\n';
});
```

客户端异步future接口

```
auto future = client->async_call<FUTURE>("echo", "hello");
auto status = future.wait_for(std::chrono::seconds(2));
if (status == std::future_status::timeout) {
	std::cout << "timeout\n";
}
else if (status == std::future_status::ready) {
	auto str = future.get().as<std::string>();
	std::cout << "echo " << str << '\n';
}
```

除了上面的这些很棒的接口之外，更酷的是rest_rpc还支持了订阅发布的功能，这是目前很多rpc库做不到的。

服务端订阅发布的例子在这里：

https://github.com/qicosmos/rest_rpc/blob/master/examples/server/main.cpp#L121
https://github.com/qicosmos/rest_rpc/blob/master/examples/client/main.cpp#L383

## 快

rest_rpc是目前最快的rpc库，具体和grpc和brpc做了性能对比测试，rest_rpc性能是最高的，远超grpc。

性能测试的结果在这里：

https://github.com/qicosmos/rest_rpc/blob/master/doc/%E5%8D%95%E6%9C%BA%E4%B8%8Arest_rpc%E5%92%8Cbrpc%E6%80%A7%E8%83%BD%E6%B5%8B%E8%AF%95.md


# rest_rpc的更多用法

可以参考rest_rpc的example:

https://github.com/qicosmos/rest_rpc/tree/master/examples

# future

make an IDL tool to genrate the client code.
