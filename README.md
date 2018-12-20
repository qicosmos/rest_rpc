# rest_rpc
modern c++11, simple, easy to use rpc framework.

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

client code

	#include <iostream>
	#include "test_client.hpp"
	#include "../codec.h"
	
	using namespace rest_rpc;
	using namespace rest_rpc::rpc_service;
	
	void test_add() {
		try{
			boost::asio::io_service io_service;
			test_client client(io_service);
			client.connect("127.0.0.1", "9000");
	
			auto result = client.call<int>("add", 1, 2);
	
			std::cout << result << std::endl; //output 3
		}
		catch (const std::exception& e){
			std::cout << e.what() << std::endl;
		}
	}
	
	void test_translate() {
		try {
			boost::asio::io_service io_service;
			test_client client(io_service);
			client.connect("127.0.0.1", "9000");
	
			auto result = client.call<std::string>("translate", "hello");
			std::cout << result << std::endl; //output HELLO
		}
		catch (const std::exception& e) {
			std::cout << e.what() << std::endl;
		}
	}
	
	void test_hello() {
		try {
			boost::asio::io_service io_service;
			test_client client(io_service);
			client.connect("127.0.0.1", "9000");
	
			client.call("hello", "purecpp");
		}
		catch (const std::exception& e) {
			std::cout << e.what() << std::endl;
		}
	}
	
	int main() {
		test_hello();
		test_add();
		test_translate();
		return 0;
	}

除了简单好用，没什么多余的要说的。

# future

make an IDL tool to genrate the client code.