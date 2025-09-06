# extra
option(BUILD_EXAMPLES "Build examples" ON)

# unit test
option(BUILD_UNIT_TESTS "Build unit tests" ON)
if(BUILD_UNIT_TESTS)
    enable_testing()
endif()

SET(ENABLE_SSL ON)
SET(ENABLE_JAVA OFF)

add_definitions(-DMSGPACK_NO_BOOST)

if (ENABLE_SSL)
    add_definitions(-DCINATRA_ENABLE_SSL)
    message(STATUS "Use SSL")
    find_package(Boost CONFIG REQUIRED COMPONENTS system filesystem )
endif()

if (ENABLE_JAVA)
    find_package(JNI REQUIRED)
    message(STATUS "Use Java")
endif()

# coverage test
option(COVERAGE_TEST "Build with unit test coverage" OFF)
if(COVERAGE_TEST)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage --coverage")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-instr-generate -fcoverage-mapping")
    endif()
endif()
