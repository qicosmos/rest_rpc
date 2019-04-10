cmake_minimum_required(VERSION 3.7)
project(app_client)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -std=c++11")

find_package(Boost COMPONENTS system REQUIRED)
include_directories(
        "../../include"
        "../../third/msgpack/include"
)

add_executable(app_client main.cpp)
target_link_libraries(app_client ${Boost_LIBRARIES})
