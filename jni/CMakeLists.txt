cmake_minimum_required(VERSION 3.1)
project(example)

set(ASIO_STANDALONE 1)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -std=c++11")

SET(ENABLE_SSL OFF)

if (ENABLE_SSL)
    add_definitions(-DCINATRA_ENABLE_SSL)
    message(STATUS "Use SSL")
endif()

find_package(JNI REQUIRED)
find_package(Boost COMPONENTS system filesystem REQUIRED)
include_directories(
        "/usr/local/include"
        "../include"
        "../../third/msgpack/include"
        "../jni"
        ${JNI_INCLUDE_DIRS})

add_library(restrpc_jni SHARED org_restrpc_client_NativeRpcClient.cc)
