project(test_rest_rpc)
set(project_name test_rest_rpc)

include_directories(
        "../thirdparty/msgpack-c/include"
        "../thirdparty/asio"
        )

#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")

add_executable(test_rest_rpc test_rest_rpc.cpp)

add_test(NAME ${project_name} COMMAND test_rest_rpc)

add_executable(bench bench.cpp)