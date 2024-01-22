project(example)

include_directories(
        "../thirdparty/msgpack-c/include"
        "../thirdparty/asio")

add_executable(basic_server server/main.cpp)
add_executable(basic_client client/main.cpp)

if (ENABLE_SSL)
    target_link_libraries(basic_server ${Boost_LIBRARIES} -lssl -lcrypto -lpthread)
    target_link_libraries(basic_client ${Boost_LIBRARIES} -lssl -lcrypto -lpthread)
else()
    target_link_libraries(basic_server ${Boost_LIBRARIES})
    target_link_libraries(basic_client ${Boost_LIBRARIES})
endif()
