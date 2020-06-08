cmake_minimum_required(VERSION 3.7)
project(basic_server)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -std=c++11")

SET(ENABLE_SSL OFF)

if (ENABLE_SSL)
    add_definitions(-DCINATRA_ENABLE_SSL)
    message(STATUS "Use SSL")
endif()

find_package(Boost COMPONENTS system filesystem REQUIRED)
include_directories(
        "../../include"
        "../../third/msgpack/include"
)

add_executable(basic_server main.cpp)

if (ENABLE_SSL)
    target_link_libraries(basic_server ${Boost_LIBRARIES} -lssl -lcrypto -lpthread)
else()
    target_link_libraries(basic_server ${Boost_LIBRARIES})
endif()
