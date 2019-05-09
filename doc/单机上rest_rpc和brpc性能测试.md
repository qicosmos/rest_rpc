单机上[rest_rpc](https://github.com/qicosmos/rest_rpc "rest_rpc")和[brpc](https://github.com/apache/incubator-brpc "brpc")性能测试

# 测试环境

 | 软硬件环境 | 参数 |  
 | -------- | :-----: | 
 | OS | 18.04.1-Ubuntu | 
 | CPU | 6Core，Intel(R) Core(TM) i5-9400F CPU @ 2.90GHz | 
 | 内存 | 16G |
 | g++版本 | g++ (Ubuntu 8.2.0-1ubuntu2~18.04) 8.2.0 |
 | 机器类别 | 实体机 | 

# 测试方法

1. server与client均为单进程并且部署在同一台实体机；
2. server开启多线程处理，线程数是CPU核数；
3. client是开启单线程与多线程分别测试性能；
4. client循环向server发送一定字节的字符串，比如由‘A’构成的1K字符串，server收到后不做业务处理，立即返回给client，client收到server返回的字符串也不做业务处理；
5. 当循环结束后，client统计延时；
6. server每隔1s会向标准输出设备打印出收到的请求数；
7. 采用rest_rpc与brpc默认的编译方式（cmake -DCMAKE_BUILD_TYPE=Release）。
8. rest_rpc采用高效模式与brpc进行对比测试。

# 测试代码编写及分析

## rest_rpc测试代码

1. 下载源码：https://github.com/qicosmos/rest_rpc
2. 解压后进入examples/server目录，修改main.cpp，增加函数：
```
std::string echo(connection* conn, const std::string& orignal) {
	g_qps.increase();
	return orignal;
}
```
3. echo函数将客户端从conn连接传过来的字符串original原封不动得返回，并调用increase函数进行请求计数。
在main函数增加

```
server.register_handler("echo", echo);
```

用于向server注册消息处理函数。

4. examples/client目录，修改main.cpp，client为单线程模式：

在test_performance1()函数中进行请求发送：
```
void test_performance1() {
	rpc_client client("127.0.0.1", 9000);
	bool r = client.connect();
	if (!r) {
		std::cout << "connect timeout" << std::endl;
		return;
	}
	string str(1024, 'A');
	auto begin = high_resolution_clock::now();
	for (size_t i = 0; i < LOOP; i++) {
		auto future = client.async_call("echo", str);
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
	auto end = high_resolution_clock::now();
	cout << "elapse time = " << duration_cast<seconds>(end - begin).count() << "s" << endl;
	std::cout << "finish\n";
}
```

client为多线程模式：
```
void test_performance1() {
	vector<thread>clients;
	auto begin = high_resolution_clock::now();
	for (int i = 0;i < THREADNUM;i++)
	{
		clients.emplace_back(thread(client_thread));
	}
	
	for (auto &it : clients)
	{
		it.join();
	}
	auto end = high_resolution_clock::now();
	cout << "elapse time = " << duration_cast<seconds>(end - begin).count() << "s" << endl;
	cout << "finish\n";
}
```

定义循环次数为： static const int LOOP = 100000;
定义线程数：const static int THREADNUM = 6;
编写client_thread：

## brpc测试代码

1. 下载源码：https://github.com/apache/incubator-brpc.git
2. 按官网编译指令下载相应包后进行编译

进入目录examples/echo_c++，修改server.cpp，在
```
virtual void Echo(google::protobuf::RpcController* cntl_base,
                      const EchoRequest* request, EchoResponse* response, google::protobuf::Closure* done)
注释相应LOG日志：
    virtual void Echo(google::protobuf::RpcController* cntl_base,
                      const EchoRequest* request,
                      EchoResponse* response,
                      google::protobuf::Closure* done) {
        // This object helps you to call done->Run() in RAII style. If you need
        // to process the request asynchronously, pass done_guard.release().
        brpc::ClosureGuard done_guard(done);

        brpc::Controller* cntl =
            static_cast<brpc::Controller*>(cntl_base);

        // The purpose of following logs is to help you to understand
        // how clients interact with servers more intuitively. You should 
        // remove these logs in performance-sensitive servers.
        /*LOG(INFO) << "Received request[log_id=" << cntl->log_id() 
                  << "] from " << cntl->remote_side() 
                  << " to " << cntl->local_side()
                  << ": " << request->message()
                  << " (attached=" << cntl->request_attachment() << ")";*/

        // Fill response.
        response->set_message(request->message());
	    g_qps.increase();
        // You can compress the response by setting Controller, but be aware
        // that compression may be costly, evaluate before turning on.
        // cntl->set_response_compress_type(brpc::COMPRESS_TYPE_GZIP);

        if (FLAGS_echo_attachment) {
            // Set attachment which is wired to network directly instead of
            // being serialized into protobuf messages.
            cntl->response_attachment().append(cntl->request_attachment());
        }
}
```
代码行response->set_message(request->message());将将客户端的请求原封不动发回客户端。

3. 修改client.cpp，单线程模式：
```
const static size_t BUF_SIZE = 1024;
const static int LOOP = 1000000;
char buf[BUF_SIZE] = ""
修改main.函数：：
int main(int argc, char* argv[]) {
	memset(buf, 'A', BUF_SIZE);
    // Parse gflags. We recommend you to use gflags as well.
    GFLAGS_NS::ParseCommandLineFlags(&argc, &argv, true);
    
    // A Channel represents a communication line to a Server. Notice that 
    // Channel is thread-safe and can be shared by all threads in your program.
    brpc::Channel channel;
    
    // Initialize the channel, NULL means using default options.
    brpc::ChannelOptions options;
	
    options.protocol = FLAGS_protocol;
    options.connection_type = FLAGS_connection_type;
    options.timeout_ms = FLAGS_timeout_ms/*milliseconds*/;
    options.max_retry = FLAGS_max_retry;
    if (channel.Init(FLAGS_server.c_str(), FLAGS_load_balancer.c_str(), &options) != 0) {
        LOG(ERROR) << "Fail to initialize channel";
        return -1;
    }

    // Normally, you should not call a Channel directly, but instead construct
    // a stub Service wrapping it. stub can be shared by all threads as well.
    example::EchoService_Stub stub(&channel);

    // Send a request and wait for the response every 1 second.
    //int log_id = 0;

	example::EchoRequest request;
	example::EchoResponse response;
	request.set_message(buf);
	auto begin = high_resolution_clock::now();
	for (int i = 0; i < LOOP; i++)
	{
		brpc::Controller cntl;
		cntl.request_attachment().append(FLAGS_attachment);
		stub.Echo(&cntl, &request, &response, NULL);
		if (!cntl.Failed()) {
			/*LOG(INFO) << "Received response from " << cntl.remote_side()
				<< " to " << cntl.local_side()
				<< ": " << response.message() << " (attached="
				<< cntl.response_attachment() << ")"
				<< " latency=" << cntl.latency_us() << "us";*/
		}
		else {
			LOG(WARNING) << cntl.ErrorText();
		}
	}
	auto end = high_resolution_clock::now();
	cout << "elapse time = " << duration_cast<seconds>(end - begin).count() << "s" << endl;
	std::cout << "finish\n";
    //LOG(INFO) << "EchoClient is going to quit";
    return 0;
}
```

去除日志，并统计循环发送延迟，在循环体内只是向server发送报文，并未执行任何业务。

多线程模式

```
#include <string.h>
#include <gflags/gflags.h>
#include <butil/logging.h>
#include <butil/time.h>
#include <brpc/channel.h>
#include <chrono>
#include <thread>
#include "echo.pb.h"
using namespace std;
using namespace chrono;
DEFINE_string(attachment, "", "Carry this along with requests");
DEFINE_string(protocol, "baidu_std", "Protocol type. Defined in src/brpc/options.proto");
DEFINE_string(connection_type, "", "Connection type. Available values: single, pooled, short");
//DEFINE_string(server, "0.0.0.0:8000", "IP Address of server");
DEFINE_string(server, "127.0.0.1:9000", "IP Address of server");
DEFINE_string(load_balancer, "", "The algorithm for load balancing");
DEFINE_int32(timeout_ms, 100, "RPC timeout in milliseconds");
DEFINE_int32(max_retry, 3, "Max retries(not including the first RPC)"); 
DEFINE_int32(interval_ms, 1000, "Milliseconds between consecutive requests");
//const static size_t BUF_SIZE = 1024 * 8 * 2 * 4;
const static size_t BUF_SIZE = 1024;
const static int LOOP = 100000;
const static int THREADNUM = 20;
char buf[BUF_SIZE] = "";
void client_thread()
{
    brpc::Channel channel;
    brpc::ChannelOptions options;
	
    options.protocol = FLAGS_protocol;
    options.connection_type = FLAGS_connection_type;
    options.timeout_ms = FLAGS_timeout_ms/*milliseconds*/;
    options.max_retry = FLAGS_max_retry;
    if (channel.Init(FLAGS_server.c_str(), FLAGS_load_balancer.c_str(), &options) != 0) {
        LOG(ERROR) << "Fail to initialize channel";
        return;
    }
    example::EchoService_Stub stub(&channel);
    example::EchoRequest request;
    example::EchoResponse response;
    request.set_message(buf);
    for (int i = 0; i < LOOP; i++)
    {
	    brpc::Controller cntl;
	    cntl.request_attachment().append(FLAGS_attachment);
	    stub.Echo(&cntl, &request, &response, NULL);
	    if (!cntl.Failed()) {
	    }
	    else {
		  LOG(WARNING) << cntl.ErrorText();
	    }
    }
}
int main(int argc, char* argv[]) {
	memset(buf, 'A', BUF_SIZE);
    GFLAGS_NS::ParseCommandLineFlags(&argc, &argv, true);
    vector<thread>clients;
    auto begin = high_resolution_clock::now();
    for (int i = 0;i < THREADNUM;i++)
    {
	    clients.emplace_back(thread(client_thread));
    }
    for (auto &it : clients)
    {
	    it.join();
    }
    auto end = high_resolution_clock::now();
    cout << "elapse time = " << duration_cast<seconds>(end - begin).count() << "s" << endl;
    cout << "finish\n";
}
```

# 测试结果

## brpc和grpc的[测试结果](https://github.com/apache/incubator-brpc/blob/master/docs/cn/benchmark.md "测试结果")

 ![alt](https://github.com/apache/incubator-brpc/blob/master/docs/images/qps_vs_threadnum.png "多线程")

## rest_rpc和brpc的测试结果

本次测试使用相同环境与方法对rest_rpc与brpc进行性能测试，最终测试数据取多次测试数据的平均值，测试数据如下：

**client使用单进程单线程测试模式**

 | 消息字节 | QPS(rest_rpc) |  QPS(brpc) |  
 | -------- | :-----: | :-----: | 
 | 1K | 52631 | 41667 |
 | 8K | 42863 | 34483 |
 | 16K | 37037 | 29412 |
 | 64K | 16949 | 8982 |
 
 ![alt](https://github.com/qicosmos/rest_rpc/blob/master/doc/%E5%8D%95%E7%BA%BF%E7%A8%8B.png "单线程")
 
从相关数据可以看出，rest_rpc QPS要高于brpc，特别是当消息字节达到64K时，rest QPS几乎是brpc的2倍。

**client多线程测试1K消息(rest_rpc采用高效模式)**

 | 线程数 | QPS(rest_rpc) |  QPS(brpc) |  
 | -------- | :-----: | :-----: | 
 | 6 | 200000 | 103448 |
 | 10 | 250000 | 166667 |
 | 20 | 285714 | 240963 |
 
  ![alt](https://github.com/qicosmos/rest_rpc/blob/master/doc/%E5%A4%9A%E7%BA%BF%E7%A8%8B1k.png "多线程1k")

**client多线程测试64K消息(rest_rpc采用高效模式)**

 | 线程数 | QPS(rest_rpc) |  QPS(brpc) |  
 | -------- | :-----: | :-----: | 
 | 6 | 60000 | 34883 |
 | 10 | 76923 | 40000 |
 | 20 | 59405 | 35242 |

 ![alt](https://github.com/qicosmos/rest_rpc/blob/master/doc/%E5%A4%9A%E7%BA%BF%E7%A8%8B64k.png "多线程64k")




