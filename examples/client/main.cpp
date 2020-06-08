#include <iostream>
#include <rpc_client.hpp>
#include <chrono>
#include <fstream>
#include "codec.h"

using namespace rest_rpc;
using namespace rest_rpc::rpc_service;

void test_add() {
	try {
		rpc_client client("127.0.0.1", 9000);
		bool r = client.connect();
		if (!r) {
			std::cout << "connect timeout" << std::endl;
			return;
		}

		{
			auto result = client.call<int>("add", 1, 2);
			std::cout << result << std::endl;
		}

		{
			auto result = client.call<2000, int>("add", 1, 2);
			std::cout << result << std::endl;
		}
	}
	catch (const std::exception & e) {
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
	catch (const std::exception & e) {
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
	catch (const std::exception & e) {
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
	catch (const std::exception & e) {
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
	catch (const std::exception & e) {
		std::cout << e.what() << std::endl;
	}
}

void test_async_client() {
	rpc_client client("127.0.0.1", 9000);
	bool r = client.connect();
	if (!r) {
		std::cout << "connect timeout" << std::endl;
		return;
	}

	client.set_error_callback([](boost::system::error_code ec) {
		std::cout << ec.message() << std::endl;
	});

	auto f = client.async_call<FUTURE>("get_person");
	if (f.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout) {
		std::cout << "timeout" << std::endl;
	}
	else {
		auto p = f.get().as<person>();
		std::cout << p.name << std::endl;
	}

	auto fu = client.async_call<FUTURE>("hello", "purecpp");
	fu.get().as(); //no return

	//sync call
	client.call("hello", "purecpp");
	auto p = client.call<person>("get_person");

	std::string str;
	std::cin >> str;
}

void test_upload() {
	rpc_client client("127.0.0.1", 9000);
	bool r = client.connect(1);
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
		auto f = client.async_call<FUTURE>("upload", "test", conent);
		if (f.wait_for(std::chrono::milliseconds(500)) == std::future_status::timeout) {
			std::cout << "timeout" << std::endl;
		}
		else {
			f.get().as();
			std::cout << "ok" << std::endl;
		}
	}
	{
		auto f = client.async_call<FUTURE>("upload", "test1", conent);
		if (f.wait_for(std::chrono::milliseconds(500)) == std::future_status::timeout) {
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
	bool r = client.connect(1);
	if (!r) {
		std::cout << "connect timeout" << std::endl;
		return;
	}

	auto f = client.async_call<FUTURE>("download", "test");
	if (f.wait_for(std::chrono::milliseconds(500)) == std::future_status::timeout) {
		std::cout << "timeout" << std::endl;
	}
	else {
		auto content = f.get().as<std::string>();
		std::cout << content.size() << std::endl;
		std::ofstream file("test", std::ios::binary);
		file.write(content.data(), content.size());
	}
}

void test_echo() {
	rpc_client client("127.0.0.1", 9000);
	bool r = client.connect();
	if (!r) {
		std::cout << "connect timeout" << std::endl;
		return;
	}

	{
		auto result = client.call<std::string>("echo", "test");
		std::cout << result << std::endl;
	}

	{
		auto result = client.call<std::string>("async_echo", "test");
		std::cout << result << std::endl;
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
		auto future = client.async_call<FUTURE>("get_name", p);
		auto status = future.wait_for(std::chrono::seconds(2));
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

void multi_client_performance(size_t n) {
	std::vector<std::shared_ptr<std::thread>> group;
	for (int i = 0; i < n; ++i) {
		group.emplace_back(std::make_shared<std::thread>(test_performance1));// []{test_performance1();});
	}
	for (auto& p : group) {
		p->join();
	}
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
	catch (const std::exception & ex) {
		std::cout << ex.what() << '\n';
	}

	std::string str;
	std::cin >> str;
}

void test_call_with_timeout() {
	rpc_client client;
	client.async_connect("127.0.0.1", 9000);

	try {
		auto result = client.call<50, person>("get_person");
		std::cout << result.name << std::endl;
		client.close();
		bool r = client.connect();
		result = client.call<50, person>("get_person");
		std::cout << result.name << std::endl;
	}
	catch (const std::exception & ex) {
		std::cout << ex.what() << std::endl;
	}

	std::string str;
	std::cin >> str;
}

void test_connect() {
	rpc_client client;
	client.enable_auto_reconnect(); //automatic reconnect
	client.enable_auto_heartbeat(); //automatic heartbeat
	bool r = client.connect("127.0.0.1", 9000);
	int count = 0;
	while (true) {
		if (client.has_connected()) {
			std::cout << "connected ok\n";
			break;
		}
		else {
			std::cout << "connected failed: "<< count++<<"\n";
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	//{
	//	rpc_client client;
	//	bool r = client.connect("127.0.0.1", 9000);
	//	int count = 0;
	//	while (true) {
	//		if (client.connect()) {
	//			std::cout << "connected ok\n";
	//			break;
	//		}
	//		else {
	//			std::cout << "connected failed: " << count++ << "\n";
	//		}
	//	}
	//}

	std::string str;
	std::cin >> str;
}

void test_callback() {
	rpc_client client;
	bool r = client.connect("127.0.0.1", 9000);

	for (size_t i = 0; i < 100; i++) {
		std::string test = "test" + std::to_string(i + 1);
		//set timeout 100ms
		client.async_call<100>("async_echo", [](const boost::system::error_code & ec, string_view data) {
			if (ec) {
				std::cout << ec.value() << " timeout" << std::endl;
				return;
			}

			auto str = as<std::string>(data);
			std::cout << "echo " << str << '\n';
		}, test);

		std::string test1 = "test" + std::to_string(i + 2);
		//zero means no timeout check, no param means using default timeout(5s)
		client.async_call<0>("echo", [](const boost::system::error_code & ec, string_view data) {
			auto str = as<std::string>(data);
			std::cout << "echo " << str << '\n';
		}, test1);
	}

	client.run();
}

void wait_for_notification(rpc_client & client) {
	client.async_call<0>("sub", [&client](const boost::system::error_code & ec, string_view data) {
		auto str = as<std::string>(data);
		std::cout << str << '\n';

		wait_for_notification(client);
	});
}

void test_sub1() {
	rpc_client client;
	client.enable_auto_reconnect();
	client.enable_auto_heartbeat();
	bool r = client.connect("127.0.0.1", 9000);
	if (!r) {
		return;
	}

	client.subscribe("key", [](string_view data) {
		std::cout << data << "\n";
	});

	client.subscribe("key", "048a796c8a3c6a6b7bd1223bf2c8cee05232e927b521984ba417cb2fca6df9d1", [](string_view data) {
		msgpack_codec codec;
		person p = codec.unpack<person>(data.data(), data.size());
		std::cout << p.name << "\n";
	});

	client.subscribe("key1", "048a796c8a3c6a6b7bd1223bf2c8cee05232e927b521984ba417cb2fca6df9d1", [](string_view data) {
		std::cout << data << "\n";
	});

	bool stop = false;
	std::thread thd1([&client, &stop] {
		while (true) {
			try {
				if (client.has_connected()) {
					int r = client.call<int>("add", 2, 3);
					std::cout << "add result: " << r << "\n";
				}
				
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			catch (const std::exception& ex) {
				std::cout << ex.what() << "\n";
			}
		}
	});

	/*rpc_client client1;
	bool r1 = client1.connect("127.0.0.1", 9000);
	if (!r1) {
		return;
	}

	person p{10, "jack", 21};
	client1.publish("key", "hello subscriber");
	client1.publish_by_token("key", "sub_key", p);
	
	std::thread thd([&client1, p] {
		while (true) {
			try {
				client1.publish("key", "hello subscriber");
				client1.publish_by_token("key", "unique_token", p);				
			}
			catch (const std::exception& ex) {
				std::cout << ex.what() << "\n";
			}
		}
	});
*/

	std::string str;
	std::cin >> str;
}

void test_multiple_thread() {
	std::vector<std::shared_ptr<rpc_client>> cls;
	std::vector<std::shared_ptr<std::thread>> v;
	for (int j = 0; j < 4; ++j) {
		auto client = std::make_shared<rpc_client>();
		cls.push_back(client);
		bool r = client->connect("127.0.0.1", 9000);
		if (!r) {
			return;
		}

		for (size_t i = 0; i < 2; i++) {
			person p{ 1, "tom", 20 };
			v.emplace_back(std::make_shared<std::thread>([client] {
				person p{ 1, "tom", 20 };
				for (size_t i = 0; i < 1000000; i++) {
					client->async_call<0>("get_name", [](const boost::system::error_code & ec, string_view data) {
						if (ec) {
							std::cout << ec.message() << '\n';
						}
					}, p);

					//auto future = client->async_call<FUTURE>("get_name", p);
					//auto status = future.wait_for(std::chrono::seconds(2));
					//if (status == std::future_status::deferred) {
					//	std::cout << "deferred\n";
					//}
					//else if (status == std::future_status::timeout) {
					//	std::cout << "timeout\n";
					//}
					//else if (status == std::future_status::ready) {
					//}

					//client->call<std::string>("get_name", p);
				}
			}));
		}
	}


	std::string str;
	std::cin >> str;
}

void test_threads() {
	rpc_client client;
	bool r = client.connect("127.0.0.1", 9000);
	if (!r) {
		return;
	}

	std::thread thd1([&client] {
		for (size_t i = 0; i < 1000000; i++) {
			auto future = client.async_call<FUTURE>("echo", "test");
			auto status = future.wait_for(std::chrono::seconds(2));
			if (status == std::future_status::timeout) {
				std::cout << "timeout\n";
			}
			else if (status == std::future_status::ready) {
				std::string content = future.get().as<std::string>();
			}

			std::this_thread::sleep_for(std::chrono::microseconds(2));
		}
		std::cout << "thread2 finished" << '\n';
	});
	
	std::thread thd2([&client] {
		for (size_t i = 1000000; i < 2*1000000; i++) {
			client.async_call("get_int", [i](boost::system::error_code ec, string_view data) {
				if (ec) {
					std::cout << ec.message() << '\n';
					return;
				}
				int r =  as<int>(data);
				if (r != i) {
					std::cout << "error not match" << '\n';
				}
			}, i);
			std::this_thread::sleep_for(std::chrono::microseconds(2));
		}

		std::cout << "thread2 finished" << '\n';
	});

	for (size_t i = 2*1000000; i < 3 * 1000000; i++) {
		auto future = client.async_call<FUTURE>("echo", "test");
		auto status = future.wait_for(std::chrono::seconds(2));
		if (status == std::future_status::timeout) {
			std::cout << "timeout\n";
		}
		else if (status == std::future_status::ready) {
			std::string content = future.get().as<std::string>();
		}
		std::this_thread::sleep_for(std::chrono::microseconds(2));
	}
	std::cout << "thread finished" << '\n';
	
	std::string str;
	std::cin >> str;
}

void test_ssl() {
    bool is_ssl = true;
    rpc_client client;
    client.set_error_callback([](boost::system::error_code ec) {
        std::cout << ec.message() << "\n";
    });

#ifdef CINATRA_ENABLE_SSL
    client.set_ssl_context_callback([](boost::asio::ssl::context& ctx) {
        ctx.set_verify_mode(boost::asio::ssl::context::verify_peer);
        ctx.load_verify_file("server.crt");
    });
#endif

    bool r = client.connect("127.0.0.1", 9000, is_ssl);
    if (!r) {
        return;
    }

    for (size_t i = 0; i < 100; i++) {
        try {
            auto result = client.call<std::string>("echo", "purecpp");
            std::cout << result << " sync\n";
        }
        catch (const std::exception& e) {
            std::cout << e.what() << " sync\n";
        }        

        auto future = client.async_call<CallModel::future>("echo", "purecpp");
        auto status = future.wait_for(std::chrono::milliseconds(5000));
        if (status == std::future_status::timeout) {
            std::cout << "timeout future\n";
        }
        else {
            auto result1 = future.get();
            std::cout << result1.as<std::string>() << " future\n";
        }

        client.async_call("echo", [](boost::system::error_code ec, string_view data) {
            if (ec) {                
                std::cout << ec.message() <<" "<< data << "\n";
                return;
            }

            auto result = as<std::string>(data);
            std::cout << result << " async\n";
        }, "purecpp");
    }

    std::getchar();
}

int main() {
	test_sub1();
	test_connect();
	test_callback();
	test_echo();
	test_sync_client();
	test_async_client();
	//test_threads();
	//test_sub();
	//test_call_with_timeout();
	//test_connect();
	//test_upload();
	//test_download();
	//multi_client_performance(20);
	//test_performance1();
	//test_multiple_thread();
	return 0;
}