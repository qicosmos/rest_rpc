cmake_minimum_required(VERSION 3.7)
project(basic_server)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -std=c++11")

find_package(Boost COMPONENTS system REQUIRED)
include_directories(
        "../../include"
        "../../third/msgpack/include"
)

add_executable(basic_server main.cpp)
target_link_libraries(basic_server ${Boost_LIBRARIES})
