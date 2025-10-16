# Compile Standard
set(CMAKE_CXX_STANDARD 20)

include_directories(
    "../thirdparty"
    "../thirdparty/asio"
)

add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/bigobj>")

# Build Type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
