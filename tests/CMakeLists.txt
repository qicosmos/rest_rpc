project(test_rest_rpc)
set(project_name test_rest_rpc)

include_directories(
        "../thirdparty/msgpack-c/include"
        "../thirdparty/asio"
        )

add_executable(${project_name}
		test_rest_rpc.cpp
        main.cpp
        )

add_test(NAME ${project_name} COMMAND test_rest_rpc)
