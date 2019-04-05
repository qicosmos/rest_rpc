#include <iostream>
#include <rpc_client.hpp>
#include <chrono>
#include <fstream>
#include "codec.h"
using namespace std::chrono_literals;

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

struct person {
	int id;
	std::string name;
	int age;

	MSGPACK_DEFINE(id, name, age);
};

void test_get_person_name() {
	try {
		rpc_client client("127.0.0.1", 9000);
		bool r = client.connect();
		if (!r) {
			std::cout << "connect timeout" << std::endl;
			return;
		}

		auto result = client.call<std::string>("get_person_name", person{ 1, "tom", 20 });
		std::cout << result << std::endl;
	}
	catch (const std::exception& e) {
		std::cout << e.what() << std::endl;
	}
}

void test_get_person() {
	try {
		rpc_client client("127.0.0.1", 9000);
		bool r = client.connect();
		if (!r) {
			std::cout << "connect timeout" << std::endl;
			return;
		}

		auto result = client.call<person>("get_person");
		std::cout << result.name << std::endl;
	}
	catch (const std::exception& e) {
		std::cout << e.what() << std::endl;
	}
}

void test_async_client() {
	rpc_client client("127.0.0.1", 9000);
	client.async_connect();
	bool r = client.wait_conn(1);
	if (!r) {
		std::cout << "connect timeout" << std::endl;
		return;
	}

	client.set_error_callback([] (boost::system::error_code ec){
		std::cout << ec.message() << std::endl;
	});

	auto f = client.async_call("get_person");
	if (f.wait_for(50ms) == std::future_status::timeout) {
		std::cout << "timeout" << std::endl;
	}
	else {
		auto p = f.get().as<person>();
		std::cout << p.name << std::endl;
	}

	auto fu = client.async_call("hello", "purecpp");
	fu.get().as(); //no return

	//sync call
	client.call("hello", "purecpp");
	auto p = client.call<person>("get_person");

	std::string str;
	std::cin >> str;
}

void test_upload() {
	rpc_client client("127.0.0.1", 9000);
	client.async_connect();
	bool r = client.wait_conn(1);
	if (!r) {
		std::cout << "connect timeout" << std::endl;
		return;
	}

	std::ifstream file("E:/acl.7z", std::ios::binary);
	file.seekg(0, std::ios::end);
	size_t file_len = file.tellg();
	file.seekg(0, std::ios::beg);
	std::string conent;
	conent.resize(file_len);
	file.read(&conent[0], file_len);
	std::cout << file_len << std::endl;

	{
		auto f = client.async_call("upload", "test", conent);
		if (f.wait_for(500ms) == std::future_status::timeout) {
			std::cout << "timeout" << std::endl;
		}
		else {
			f.get().as();
			std::cout << "ok" << std::endl;
		}
	}
	{
		auto f = client.async_call("upload", "test1", conent);
		if (f.wait_for(500ms) == std::future_status::timeout) {
			std::cout << "timeout" << std::endl;
		}
		else {
			f.get().as();
			std::cout << "ok" << std::endl;
		}
	}
}

void test_download() {
	rpc_client client("127.0.0.1", 9000);
	client.async_connect();
	bool r = client.wait_conn(1);
	if (!r) {
		std::cout << "connect timeout" << std::endl;
		return;
	}

	auto f = client.async_call("download", "test");
	if (f.wait_for(500ms) == std::future_status::timeout) {
		std::cout << "timeout" << std::endl;
	}
	else {
		auto content = f.get().as<std::string>();
		std::cout << content.size() << std::endl;
		std::ofstream file("test", std::ios::binary);
		file.write(content.data(), content.size());
	}
}

void test_sync_client() {
	test_add();
	test_translate();
	test_hello();
	test_get_person_name();
	test_get_person();
}

void test_performance1() {
	rpc_client client("127.0.0.1", 9000);
	bool r = client.connect();
	if (!r) {
		std::cout << "connect timeout" << std::endl;
		return;
	}

	person p{ 1, "tom", 20 };

	for (size_t i = 0; i < 100000000; i++) {
		auto future = client.async_call("get_name", p);
		auto status = future.wait_for(2s);
		if (status == std::future_status::deferred) {
			std::cout << "deferred\n";
		}
		else if (status == std::future_status::timeout) {
			std::cout << "timeout\n";
		}
		else if (status == std::future_status::ready) {
		}
	}

	std::cout << "finish\n";
	
	std::string str;
	std::cin >> str;
}

void test_performance2() {
	rpc_client client("127.0.0.1", 9000);
	bool r = client.connect();
	if (!r) {
		std::cout << "connect timeout" << std::endl;
		return;
	}

	person p{ 1, "tom", 20 };

	try {
		for (size_t i = 0; i < 100000000; i++) {
			client.call<std::string>("get_name", p);
		}
		std::cout << "finish\n";
	}
	catch (const std::exception& ex) {
		std::cout << ex.what() << '\n';
	}
	
	std::string str;
	std::cin >> str;
}

int main() {
	test_performance1();
	test_sync_client();
	test_async_client();
	//test_upload();
	//test_download();
	//test_performance1();
	return 0;
}