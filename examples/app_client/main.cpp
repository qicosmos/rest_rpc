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