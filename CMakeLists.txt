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
    src/io/udp_socket.cpp
)

target_include_directories(pensieve_io PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pensieve_io PUBLIC PkgConfig::URING)

# ---------------------------------------------------------------------------
# Hash library
# ---------------------------------------------------------------------------
add_library(pensieve_hash
    src/hash/murmurhash3.cpp
)

target_include_directories(pensieve_hash PUBLIC ${CMAKE_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Membership library
# ---------------------------------------------------------------------------
add_library(pensieve_membership
    src/membership/member_list.cpp
    src/membership/swim_message.cpp
    src/membership/disseminator.cpp
    src/membership/ring_store.cpp
    src/membership/ring_migration.cpp
    src/membership/swim_protocol.cpp
)

target_include_directories(pensieve_membership PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pensieve_membership PUBLIC pensieve_io pensieve_hash)

# ---------------------------------------------------------------------------
# Storage library
# ---------------------------------------------------------------------------
add_library(pensieve_storage
    src/storage/slab_allocator.cpp
    src/storage/local_store.cpp
    src/storage/clock_evictor.cpp
)

target_include_directories(pensieve_storage PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pensieve_storage PUBLIC pensieve_hash)

# ---------------------------------------------------------------------------
# Coordinator library
# ---------------------------------------------------------------------------
add_library(pensieve_coordinator
    src/coordinator/coordinator.cpp
    src/coordinator/peer_client.cpp
    src/coordinator/wait_group.cpp
)

target_include_directories(pensieve_coordinator PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pensieve_coordinator PUBLIC
    pensieve_io pensieve_membership pensieve_storage)

# ---------------------------------------------------------------------------
# SDK library (no io_uring dependency -- portable POSIX sockets)
# ---------------------------------------------------------------------------
add_library(pensieve_sdk
    src/sdk/topology_manager.cpp
    src/sdk/connection_pool.cpp
    src/sdk/circuit_breaker.cpp
    src/sdk/pensieve_client.cpp
)

target_include_directories(pensieve_sdk PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pensieve_sdk PUBLIC pensieve_hash)

# ---------------------------------------------------------------------------
# Server binary
# ---------------------------------------------------------------------------
add_executable(pensieve_server src/main.cpp)
target_link_libraries(pensieve_server PRIVATE
    pensieve_coordinator pensieve_storage
    pensieve_membership pensieve_hash pensieve_io)

# ---------------------------------------------------------------------------
# CLI binary
# ---------------------------------------------------------------------------
add_executable(pensieve_cli src/sdk/cli.cpp)
target_link_libraries(pensieve_cli PRIVATE pensieve_sdk)

# ---------------------------------------------------------------------------
# Benchmark binary
# ---------------------------------------------------------------------------
add_executable(pensieve_bench src/sdk/benchmark.cpp)
target_link_libraries(pensieve_bench PRIVATE pensieve_sdk)

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
pensieve_add_test(test_e2e              tests/io/test_e2e.cpp)
pensieve_add_test(test_udp_socket       tests/io/test_udp_socket.cpp)

# Membership tests
function(pensieve_add_membership_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE pensieve_membership GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

pensieve_add_membership_test(test_node_info    tests/membership/test_node_info.cpp)
pensieve_add_membership_test(test_member_list  tests/membership/test_member_list.cpp)
pensieve_add_membership_test(test_swim_message  tests/membership/test_swim_message.cpp)
pensieve_add_membership_test(test_disseminator  tests/membership/test_disseminator.cpp)
pensieve_add_membership_test(test_ring_store     tests/membership/test_ring_store.cpp)
pensieve_add_membership_test(test_swim_protocol    tests/membership/test_swim_protocol.cpp)
pensieve_add_membership_test(test_swim_e2e         tests/membership/test_swim_e2e.cpp)
pensieve_add_membership_test(test_ring_migration   tests/membership/test_ring_migration.cpp)

# Hash tests
function(pensieve_add_hash_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE pensieve_hash pensieve_membership GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

pensieve_add_hash_test(test_murmurhash3     tests/hash/test_murmurhash3.cpp)
pensieve_add_hash_test(test_hasher          tests/hash/test_hasher.cpp)
pensieve_add_hash_test(test_virtual_nodes   tests/hash/test_virtual_nodes.cpp)
pensieve_add_hash_test(test_ring_lookup     tests/hash/test_ring_lookup.cpp)

# Storage tests
function(pensieve_add_storage_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE pensieve_storage GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

pensieve_add_storage_test(test_slab_allocator  tests/storage/test_slab_allocator.cpp)
pensieve_add_storage_test(test_local_store     tests/storage/test_local_store.cpp)
pensieve_add_storage_test(test_clock_evictor   tests/storage/test_clock_evictor.cpp)
pensieve_add_storage_test(test_cache_line      tests/storage/test_cache_line.cpp)

# Coordinator tests
function(pensieve_add_coordinator_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE pensieve_coordinator GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

pensieve_add_coordinator_test(test_protocol     tests/coordinator/test_protocol.cpp)
pensieve_add_coordinator_test(test_wait_group   tests/coordinator/test_wait_group.cpp)
pensieve_add_coordinator_test(test_coordinator  tests/coordinator/test_coordinator.cpp)

# SDK tests
function(pensieve_add_sdk_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE pensieve_sdk GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

pensieve_add_sdk_test(test_circuit_breaker   tests/sdk/test_circuit_breaker.cpp)

# SDK integration tests require the coordinator (for serving ClusterInfo)
function(pensieve_add_sdk_integration_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE pensieve_sdk pensieve_coordinator GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

pensieve_add_sdk_integration_test(test_sdk_e2e tests/sdk/test_sdk_e2e.cpp)
