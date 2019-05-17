#include <rpc_server.h>
using namespace rest_rpc;
using namespace rpc_service;
#include <fstream>

#include "qps.h"

struct dummy{
	int add(rpc_conn conn, int a, int b) { return a + b; }
};

std::string translate(rpc_conn conn, const std::string& orignal) {
	std::string temp = orignal;
	for (auto& c : temp) { 
		c = std::toupper(c); 
	}
	return temp;
}

void hello(rpc_conn conn, const std::string& str) {
	std::cout << "hello " << str << std::endl;
}

struct person {
	int id;
	std::string name;
	int age;

	MSGPACK_DEFINE(id, name, age);
};

std::string get_person_name(rpc_conn conn, const person& p) {
	return p.name;
}

person get_person(rpc_conn conn) {
	return { 1, "tom", 20 };
}

void upload(rpc_conn conn, const std::string& filename, const std::string& content) {
	std::cout << content.size() << std::endl;
	std::ofstream file(filename, std::ios::binary);
	file.write(content.data(), content.size());
}

std::string download(rpc_conn conn, const std::string& filename) {
	std::ifstream file(filename, std::ios::binary);
	if (!file) {
		return "";
	}

	file.seekg(0, std::ios::end);
	size_t file_len = file.tellg();
	file.seekg(0, std::ios::beg);
	std::string content;
	content.resize(file_len);
	file.read(&content[0], file_len);
	std::cout << file_len << std::endl;

	return content;
}

qps g_qps;

std::string get_name(rpc_conn conn, const person& p) {
	g_qps.increase();
	return p.name;
}

//if you want to response later, you can use async model, you can control when to response
void async_echo(rpc_conn conn, const std::string& src) {
	std::thread thd([conn, src] {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		auto conn_sp = conn.lock();
		if (conn_sp) {
			conn_sp->pack_and_response(std::move(src));
		}
	});
	thd.detach();
}

std::string echo(rpc_conn conn, const std::string& src) {
	return src;
}

int main() {
	rpc_server server(9000, std::thread::hardware_concurrency());

	dummy d;
	server.register_handler("add", &dummy::add, &d);
	server.register_handler("translate", translate);
	server.register_handler("hello", hello);
	server.register_handler("get_person_name", get_person_name);
	server.register_handler("get_person", get_person);
	server.register_handler("upload", upload);
	server.register_handler("download", download);
	server.register_handler("get_name", get_name);
	server.register_handler<ExecMode::async>("async_echo", async_echo);
	server.register_handler("echo", echo);

	server.run();

	std::string str;
	std::cin >> str;
}