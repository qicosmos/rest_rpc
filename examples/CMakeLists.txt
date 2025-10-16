project(example)
set(project_name test_rest_rpc)

include_directories(
        "../thirdparty/asio"
        "../thirdparty"
        )

add_executable(server server.cpp)
add_executable(client client.cpp)
