# rest_rpc
c++11, high performance, simple, easy to use rpc framework.

It's so easy to love RPC.

Modern C++开发的RPC库就是这么简单好用！

# quick example

server code

	#include "rpc_server.h"
	using namespace rest_rpc;
	using namespace rpc_service;
	
	struct dummy{
		int add(connection* conn, int a, int b) { return a + b; }
	};
	
	std::string translate(connection* conn, const std::string& orignal) {
		std::string temp = orignal;
		for (auto& c : temp) { 
			c = std::toupper(c); 
		}
		return temp;
	}
	
	void hello(connection* conn, const std::string& str) {
		std::cout << "hello " << str << std::endl;
	}
	
	int main() {
		rpc_server server(9000, 4);
	
		dummy d;
		server.register_handler("add", &dummy::add, &d);
		server.register_handler("translate", translate);
		server.register_handler("hello", hello);
	
		server.run();
	
		std::string str;
		std::cin >> str;
	}

通用的rpc_client用法_

	#include <iostream>
    #include <rpc_client.hpp>
    #include <chrono>
    #include <fstream>
    #include "codec.h"

    using namespace rest_rpc;
    using namespace rest_rpc::rpc_service;

    void test_add() {
	    try{
		    rpc_client client("127.0.0.1", 9000);
		    bool r = client.connect();
		    if (!r) {
			    std::cout << "connect timeout" << std::endl;
			    return;
		    }

		    auto result = client.call<int>("add", 1, 2);
		    std::cout << result << std::endl;
	    }
	    catch (const std::exception& e){
		    std::cout << e.what() << std::endl;
	    }
    }

    void test_translate() {
	    try {
		    rpc_client client("127.0.0.1", 9000);
		    bool r = client.connect();
		    if (!r) {
			    std::cout << "connect timeout" << std::endl;
			    return;
		    }

		    auto result = client.call<std::string>("translate", "hello");
		    std::cout << result << std::endl;
	    }
	    catch (const std::exception& e) {
		    std::cout << e.what() << std::endl;
	    }
    }

    void test_hello() {
	    try {
		    rpc_client client("127.0.0.1", 9000);
		    bool r = client.connect();
		    if (!r) {
			    std::cout << "connect timeout" << std::endl;
			    return;
		    }

		    client.call("hello", "purecpp");
	    }
	    catch (const std::exception& e) {
		    std::cout << e.what() << std::endl;
	    }
    }
	
	int main() {
		test_add();
	    test_translate();
	    test_hello();
		return 0;
	}

如果希望client使用的时候更加安全，可以自己对通用的rpc_client做一个简单的封装，就可以更安全的使用接口了，具体可以参考example中的app_client.
在app_client中不需要输入rpc服务名称，需要输入的参数也做了类型限定，可以保证不会传入错误的参数。

app_client的用法

    #include <iostream>
    #include "client.hpp"

    void test_client() {
	    client cl("127.0.0.1", 9000);
	    bool r = cl.connect();
	    if (!r) {
		    std::cout << "connect failed" << std::endl;
		    return;
	    }

	    try {
		    int result = cl.add(2, 3);
		    cl.hello("purecpp");
		    auto str = cl.translate("purecpp");
		    auto name = cl.get_person_name({ 1, "tom", 20 });
		    auto p = cl.get_person();

		    std::cout << result << '\n';
		    std::cout << str << '\n';
		    std::cout << name << '\n';
		    std::cout << p.name << '\n';

		    {
			    auto future = cl.async_add(4, 5);
			    if (future.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout) {
				    std::cout << "timeout" << std::endl;
			    }
			    else {
				    auto result = future.get().as<int>();
				    std::cout << result << std::endl;
			    }
		    }

		    {
			    auto future = cl.async_translate("modern c++");
			    if (future.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout) {
				    std::cout << "timeout" << std::endl;
			    }
			    else {
				    auto result = future.get().as<std::string>();
				    std::cout << result << std::endl;
			    }
		    }
	    }
	    catch (const std::exception& ex) {
		    std::cout << ex.what() << std::endl;
	    }
    }

    int main() {
	    test_client();

	    std::string str;
	    std::cin >> str;
    }


除了简单好用，没什么多余的要说的。

# future

make an IDL tool to genrate the client code.