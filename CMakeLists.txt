cmake_minimum_required(VERSION 3.22)
project(pensieve LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# liburing
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(pensieve_io
    src/io/io_uring_context.cpp
    src/io/tcp_listener.cpp
    src/io/tcp_connection.cpp
    src/io/buffer_pool.cpp
)

target_include_directories(pensieve_io PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pensieve_io PUBLIC PkgConfig::URING)

# ---------------------------------------------------------------------------
# Tests (GTest via FetchContent)
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

function(pensieve_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE pensieve_io GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

pensieve_add_test(test_io_uring_context tests/io/test_io_uring_context.cpp)
pensieve_add_test(test_buffer_pool      tests/io/test_buffer_pool.cpp)
pensieve_add_test(test_tcp_echo         tests/io/test_tcp_echo.cpp)
pensieve_add_test(test_coroutine        tests/io/test_coroutine.cpp)
