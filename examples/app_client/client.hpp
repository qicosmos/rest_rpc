#pragma once
#include <rpc_client.hpp>
using namespace rest_rpc;
using namespace rest_rpc::rpc_service;

struct person {
	int id;
	std::string name;
	int age;

	MSGPACK_DEFINE(id, name, age);
};

class client {
public:
	client(std::string ip, short port) : client_(std::move(ip), port) {
	}

	bool connect() {
		return client_.connect();
	}

	void async_connect() {
		client_.async_connect();
	}

	bool wait_conn(size_t timeout) {
		return client_.wait_conn(timeout);
	}

	int add(int a, int b) {
		try {
			return client_.call<int>("add", a, b);
		}
		catch (const std::exception & e) {
			std::cout << e.what() << std::endl;
			return -1;
		}
	}

	std::future<req_result> async_add(int a, int b) {
		return client_.async_call<FUTURE>("add", a, b);
	}

	std::string translate(const std::string& str) {
		try {
			return client_.call<std::string>("translate", str);
		}
		catch (const std::exception & e) {
			std::cout << e.what() << std::endl;
			return "";
		}
	}

	std::future<req_result> async_translate(const std::string& str) {
		return client_.async_call<FUTURE>("translate", str);
	}

	void hello(const std::string& str) {
		try {
			client_.call("hello", str);
		}
		catch (const std::exception & e) {
			std::cout << e.what() << std::endl;
		}
	}

	std::string get_person_name(const person& p) {
		try {
			return client_.call<std::string>("get_person_name", p);
		}
		catch (const std::exception & e) {
			std::cout << e.what() << std::endl;
			return "";
		}
	}

	person get_person() {
		try {
			return client_.call<person>("get_person");
		}
		catch (const std::exception & e) {
			std::cout << e.what() << std::endl;
			return {};
		}
	}
private:
	rpc_client client_;
};