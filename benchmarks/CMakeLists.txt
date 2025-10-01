cmake_minimum_required(VERSION 3.22)

# Benchmark module for Aeronet
# Only built when AERONET_BUILD_BENCHMARKS=ON (default only for main project).
# Intentionally skipped in CI unless explicitly forced (benchmarks are for local perf work).

option(AERONET_BENCH_FORCE_CI "Force building benchmarks even when CI env var is set" OFF)
option(AERONET_BENCH_ENABLE_OATPP "Fetch and build oatpp for comparative benchmarks" OFF)
option(AERONET_BENCH_ENABLE_DROGON "Fetch and build drogon for comparative benchmarks" OFF)

if(DEFINED ENV{CI} AND NOT AERONET_BENCH_FORCE_CI)
    message(STATUS "[aeronet][bench] CI environment detected; skipping benchmark targets (set AERONET_BENCH_FORCE_CI=ON to override)")
    return()
endif()

include(FetchContent)

# Google Benchmark
if(NOT TARGET benchmark)
    FetchContent_Declare(
        google_benchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.9.4.tar.gz
        URL_HASH SHA256=b334658edd35efcf06a99d9be21e4e93e092bd5f95074c1673d5c8705d95c104
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(google_benchmark)
endif()

# Optional comparative libraries (OFF by default to keep local setup lightweight).
if(AERONET_BENCH_ENABLE_OATPP)
    FetchContent_Declare(
        oatpp
        GIT_REPOSITORY https://github.com/oatpp/oatpp.git
        GIT_TAG 1.3.0
    )
    set(OATPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(oatpp)
endif()

if(AERONET_BENCH_ENABLE_DROGON)
    # drogon depends on trantor; FetchContent will pull submodules if we enable them.
    FetchContent_Declare(
        drogon
        GIT_REPOSITORY https://github.com/drogonframework/drogon.git
        GIT_TAG v1.9.7
    )
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(drogon)
endif()

set(AERONET_BENCH_INTERNAL_SOURCES
    internal/bench_request_parse.cpp
)

# A tiny interface library to share selected test utilities with benchmarks.
add_library(aeronet_bench_util INTERFACE)
target_include_directories(aeronet_bench_util INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/internal
    ${CMAKE_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/tests
)
add_library(aeronet::bench_util ALIAS aeronet_bench_util)
if(TARGET aeronet)
    target_link_libraries(aeronet_bench_util INTERFACE aeronet) # for socket/ns dependencies
endif()

add_executable(aeronet-bench-internal ${AERONET_BENCH_INTERNAL_SOURCES})
if(TARGET aeronet)
    target_link_libraries(aeronet-bench-internal PRIVATE aeronet benchmark::benchmark aeronet_bench_util)
    target_include_directories(aeronet-bench-internal PRIVATE ${CMAKE_SOURCE_DIR}/tests)
else()
    message(WARNING "[aeronet][bench] 'aeronet' target not found; internal microbench will be broken")
endif()
set_target_properties(aeronet-bench-internal PROPERTIES FOLDER "benchmarks")

add_custom_target(run-aeronet-bench
    COMMAND aeronet-bench-internal --benchmark_report_aggregates_only=true
    DEPENDS aeronet-bench-internal
    COMMENT "Running aeronet internal microbenchmarks"
)

# E2E throughput skeleton (placeholder; real workload in separate source)
if(TARGET aeronet)
    add_executable(aeronet-bench-throughput e2e/bench_throughput_local.cpp)
    target_link_libraries(aeronet-bench-throughput PRIVATE aeronet)
    set_target_properties(aeronet-bench-throughput PROPERTIES FOLDER "benchmarks")
    add_custom_target(run-aeronet-bench-throughput
        COMMAND aeronet-bench-throughput
        DEPENDS aeronet-bench-throughput
        COMMENT "Running Aeronet local throughput benchmark (skeleton)"
    )
else()
    message(WARNING "[aeronet][bench] skipping throughput benchmark; 'aeronet' target not defined")
endif()
