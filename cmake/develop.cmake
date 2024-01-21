# extra
option(BUILD_EXAMPLES "Build examples" ON)

# unit test
option(BUILD_UNIT_TESTS "Build unit tests" ON)
if(BUILD_UNIT_TESTS)
    enable_testing()
endif()

SET(ENABLE_SSL OFF)
SET(ENABLE_JAVA OFF)

add_definitions(-DMSGPACK_NO_BOOST)

if (ENABLE_SSL)
    add_definitions(-DCINATRA_ENABLE_SSL)
    message(STATUS "Use SSL")
    find_package(Boost COMPONENTS system filesystem REQUIRED)
endif()

if (ENABLE_JAVA)
    find_package(JNI REQUIRED)
    message(STATUS "Use Java")
endif()
