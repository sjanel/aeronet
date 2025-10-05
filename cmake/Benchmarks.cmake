# Benchmarks.cmake - encapsulates all benchmark-related build logic for aeronet.
# Included from top-level CMakeLists.txt when AERONET_BUILD_BENCHMARKS=ON.

if(TARGET aeronet-bench-internal)
  return() # Already configured
endif()

option(AERONET_BENCH_FORCE_CI "Force building benchmarks even when CI env var is set" OFF)
option(AERONET_BENCH_ENABLE_OATPP "Fetch and build oatpp for comparative benchmarks" OFF)
option(AERONET_BENCH_ENABLE_DROGON "Fetch and build drogon for comparative benchmarks" OFF)

if(DEFINED ENV{CI} AND NOT AERONET_BENCH_FORCE_CI)
  message(STATUS "[aeronet][bench] CI detected; skipping benchmarks (override with -DAERONET_BENCH_FORCE_CI=ON)")
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

# Optional comparative libs
if(AERONET_BENCH_ENABLE_OATPP)
  FetchContent_Declare(
    oatpp
    GIT_REPOSITORY https://github.com/oatpp/oatpp.git
    GIT_TAG 1.3.0
  )
  set(OATPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(OATPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(oatpp)
endif()

if(AERONET_BENCH_ENABLE_DROGON)
  FetchContent_Declare(
    drogon
    GIT_REPOSITORY https://github.com/drogonframework/drogon.git
    GIT_TAG v1.9.11
  )
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(drogon)
endif()

# Resolve benchmark root (directory containing benchmark sources)
# We anchor paths on CMAKE_SOURCE_DIR to avoid accidental truncation when this
# file is included from the top-level (CMAKE_CURRENT_SOURCE_DIR would already
# be the project root, so using "../benchmarks" wrongly escaped the project root
# producing /path/to/..../benchmarks without the aeronet segment).
set(AERONET_BENCH_ROOT "${CMAKE_SOURCE_DIR}/benchmarks")

if(NOT EXISTS "${AERONET_BENCH_ROOT}")
  message(FATAL_ERROR "[aeronet][bench] Benchmark root directory not found at ${AERONET_BENCH_ROOT}")
endif()

# Internal microbenchmarks
set(AERONET_BENCH_INTERNAL_SOURCES
  ${AERONET_BENCH_ROOT}/internal/bench_request_parse.cpp
)

# Internal microbenchmarks (Google Benchmark main contained in bench_request_parse.cpp)
add_project_executable(aeronet-bench-internal ${AERONET_BENCH_INTERNAL_SOURCES})
if(TARGET aeronet)
  target_link_libraries(aeronet-bench-internal PRIVATE aeronet benchmark::benchmark)
else()
  message(WARNING "[aeronet][bench] 'aeronet' target not found; aeronet-bench-internal will fail to link")
endif()
set_target_properties(aeronet-bench-internal PROPERTIES FOLDER "benchmarks")

# Throughput benchmark (simple skeleton; not using Google Benchmark intentionally)
if(TARGET aeronet)
  add_project_executable(aeronet-bench-throughput ${AERONET_BENCH_ROOT}/e2e/bench_throughput_local.cpp)
  target_link_libraries(aeronet-bench-throughput PRIVATE aeronet benchmark::benchmark)
  set_target_properties(aeronet-bench-throughput PROPERTIES FOLDER "benchmarks")
endif()

# Comparative framework benchmark (aeronet vs optional drogon/oatpp)
set(AERONET_BENCH_FRAMEWORKS_SOURCES ${AERONET_BENCH_ROOT}/frameworks/bench_frameworks_basic.cpp)
add_project_executable(aeronet-bench-frameworks ${AERONET_BENCH_FRAMEWORKS_SOURCES})
target_link_libraries(aeronet-bench-frameworks PRIVATE aeronet benchmark::benchmark)
target_include_directories(aeronet-bench-frameworks PRIVATE ${CMAKE_SOURCE_DIR}/tests)
if(AERONET_BENCH_ENABLE_DROGON AND TARGET drogon)
  target_link_libraries(aeronet-bench-frameworks PRIVATE drogon)
  target_compile_definitions(aeronet-bench-frameworks PRIVATE HAVE_BENCH_DROGON)
endif()
if(AERONET_BENCH_ENABLE_OATPP)
  # Oatpp defines multiple components; core + web + others are transitively linked by target oatpp::oatpp
  if(TARGET oatpp::oatpp)
    target_link_libraries(aeronet-bench-frameworks PRIVATE oatpp::oatpp)
    target_compile_definitions(aeronet-bench-frameworks PRIVATE HAVE_BENCH_OATPP)
  else()
    message(WARNING "[aeronet][bench] Oatpp target not found after FetchContent")
  endif()
endif()
set_target_properties(aeronet-bench-frameworks PROPERTIES FOLDER "benchmarks")

# Convenience run targets
add_custom_target(run-aeronet-bench
  COMMAND aeronet-bench-internal --benchmark_report_aggregates_only=true
  DEPENDS aeronet-bench-internal
  COMMENT "Running aeronet internal microbenchmarks")

if(TARGET aeronet-bench-throughput)
  add_custom_target(run-aeronet-bench-throughput
    COMMAND aeronet-bench-throughput
    DEPENDS aeronet-bench-throughput
    COMMENT "Running aeronet throughput skeleton benchmark")
endif()

## (Removed) run-aeronet-bench-frameworks convenience target after converting to Google Benchmark.

# JSON output helper for Google Benchmark (writes a JSON file under build dir)
add_custom_target(run-aeronet-bench-json
  COMMAND aeronet-bench-internal --benchmark_format=json > aeronet-benchmarks.json
  DEPENDS aeronet-bench-internal
  COMMENT "Running aeronet benchmarks (JSON output -> aeronet-benchmarks.json)")
