#include <iostream>
#include "test_client.hpp"
#include "async_client.hpp"
#include "../codec.h"

using namespace rest_rpc;
using namespace rest_rpc::rpc_service;

void test_add() {
	try{
		boost::asio::io_service io_service;
		test_client client(io_service);
		client.connect("127.0.0.1", "9000");

		auto result = client.call<int>("add", 1, 2);

		std::cout << result << std::endl;
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
		std::cout << result << std::endl;
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

struct person {
	int id;
	std::string name;
	int age;

	MSGPACK_DEFINE(id, name, age);
};

void test_get_person_name() {
	try {
		boost::asio::io_service io_service;
		test_client client(io_service);
		client.connect("127.0.0.1", "9000");

		auto result = client.call<std::string>("get_person_name", person{ 1, "tom", 20 });
		std::cout << result << std::endl;
	}
	catch (const std::exception& e) {
		std::cout << e.what() << std::endl;
	}
}

void test_get_person() {
	try {
		boost::asio::io_service io_service;
		test_client client(io_service);
		client.connect("127.0.0.1", "9000");

		auto result = client.call<person>("get_person");
		std::cout << result.name << std::endl;
	}
	catch (const std::exception& e) {
		std::cout << e.what() << std::endl;
	}
}

void test_async_client() {
	async_client client("127.0.0.1", 9000);
	client.connect();
	std::string str1;
	std::cin >> str1;

	//call contract,firstly set all callback functions
	client.set_error_callback([] (auto ec){
		std::cout << ec.message() << std::endl;
	});

	client.set_callback("hello", [](const char* data, size_t size) {
		std::cout << "hello" << std::endl;
	});

	client.set_callback("get_person", [](const char* data, size_t size) {
		std::cout << "hello" << std::endl;
	});

	client.call("hello", "purecpp");

	std::string str;
	std::cin >> str;
}

int main() {
	test_async_client();
	test_get_person();
	test_get_person_name();
	test_hello();
	test_add();
	test_translate();
	return 0;
}