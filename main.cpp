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