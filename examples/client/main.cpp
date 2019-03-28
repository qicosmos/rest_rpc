#include <iostream>
#include <sync_client.hpp>
#include <async_client.hpp>
#include <fstream>
#include "codec.h"
using namespace std::chrono_literals;

using namespace rest_rpc;
using namespace rest_rpc::rpc_service;

void test_add() {
	try{
		boost::asio::io_service io_service;
		sync_client client(io_service);
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
		sync_client client(io_service);
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
		sync_client client(io_service);
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
		sync_client client(io_service);
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
		sync_client client(io_service);
		client.connect("127.0.0.1", "9000");

		auto result = client.call<person>("get_person");
		std::cout << result.name << std::endl;
	}
	catch (const std::exception& e) {
		std::cout << e.what() << std::endl;
	}
}

struct dummy {
	void foo(boost::system::error_code ec, std::string_view data) {
		if (ec) {
			std::cout << "ec" << std::endl;
			return;
		}
		std::cout << data.size() << std::endl;
	}
};

void test_async_client() {
	async_client client("127.0.0.1", 9000);
	client.connect();
	bool r = client.wait_conn(1);
	if (!r) {
		std::cout << "connect timeout" << std::endl;
		return;
	}

	client.set_error_callback([] (boost::system::error_code ec){
		std::cout << ec.message() << std::endl;
	});

	dummy dm;
	client.async_call("hello", "purecpp", std::bind(&dummy::foo, &dm, std::placeholders::_1,
		std::placeholders::_2), 2000);

	client.async_call("hello", "purecpp", [](auto ec, auto data) {
		if (ec) {
			std::cout << "ec" << std::endl;
			return;
		}

		if (has_error(data)) {
			std::cout << "rpc error" << std::endl;
			return;
		}

		std::cout << data.size() << std::endl;
	}, 2000);

	client.async_call("hello", "purecpp", [](auto ec, auto data) {
		if (ec) {
			std::cout << "ec" << std::endl;
			return;
		}
		std::cout << data.size() << std::endl;
	});

	client.async_call("get_person", [](auto ec, auto data) {
		if (ec) {
			std::cout << "ec" << std::endl;
			return;
		}
		std::cout << data.size() << std::endl;
	});

	client.async_call("get_person", [](auto ec, auto data) {
		if (ec) {
			std::cout << "ec" << std::endl;
			return;
		}
		std::cout << data.size() << std::endl;
	}, 2000);

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
	async_client client("127.0.0.1", 9000);
	client.connect();
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
	async_client client("127.0.0.1", 9000);
	client.connect();
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

int main() {
	//test_upload();
	//test_download();
	
	test_async_client();
	test_get_person();
	test_get_person_name();
	test_hello();
	test_add();
	test_translate();
	return 0;
}