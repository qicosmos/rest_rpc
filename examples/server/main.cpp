#include <rpc_server.h>
using namespace rest_rpc;
using namespace rpc_service;
#include <fstream>
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

struct person {
	int id;
	std::string name;
	int age;

	MSGPACK_DEFINE(id, name, age);
};

std::string get_person_name(connection* conn, const person& p) {
	return p.name;
}

person get_person(connection* conn) {
	return { 1, "tom", 20 };
}

void upload(connection* conn, const std::string& filename, const std::string& content) {
	std::cout << content.size() << std::endl;
	std::ofstream file(filename, std::ios::binary);
	file.write(content.data(), content.size());
}

std::string download(connection* conn, const std::string& filename) {
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

int main() {
	rpc_server server(9000, 4);

	dummy d;
	server.register_handler("add", &dummy::add, &d);
	server.register_handler("translate", translate);
	server.register_handler("hello", hello);
	server.register_handler("get_person_name", get_person_name);
	server.register_handler("get_person", get_person);
	server.register_handler("upload", upload);
	server.register_handler("download", download);

	server.run();

	std::string str;
	std::cin >> str;
}