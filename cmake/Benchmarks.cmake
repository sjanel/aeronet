# Benchmarks.cmake - encapsulates all benchmark-related build logic for aeronet.
option(AERONET_BENCH_ENABLE_OATPP "Fetch and build oatpp for comparative benchmarks" ON)
option(AERONET_BENCH_ENABLE_DROGON "Fetch and build drogon for comparative benchmarks" ON)
option(AERONET_BENCH_ENABLE_HTTPLIB "Fetch and build cpp-httplib for comparative benchmarks" ON)
option(AERONET_BENCH_ENABLE_PISTACHE "Build pistache benchmark server if pistache is found" ON)
option(AERONET_BENCH_ENABLE_CROW "Build Crow benchmark server (CrowCpp/Crow)" ON)

include(FetchContent)

# Google Benchmark
if(NOT TARGET benchmark)
  FetchContent_Declare(
    google_benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.5.tar.gz
    URL_HASH SHA256=9631341c82bac4a288bef951f8b26b41f69021794184ece969f8473977eaa340
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(google_benchmark)
endif()

# Optional comparative libs
if(AERONET_BENCH_ENABLE_OATPP)
  FetchContent_Declare(
    oatpp
    URL https://github.com/oatpp/oatpp/archive/refs/tags/1.3.1.tar.gz
    URL_HASH SHA256=9dd31f005ab0b3e8895a478d750d7dbce99e42750a147a3c42a9daecbddedd64
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(OATPP_INSTALL OFF CACHE BOOL "" FORCE)
  set(OATPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(OATPP_LINK_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(OATPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(oatpp)
endif()

if(AERONET_BENCH_ENABLE_DROGON)
  FetchContent_Declare(
    drogon
    GIT_REPOSITORY https://github.com/drogonframework/drogon.git
    GIT_TAG v1.9.12
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    # Ensure the 'trantor' submodule is fetched so add_subdirectory(trantor) succeeds
    GIT_SUBMODULES "trantor"
    GIT_SUBMODULES_RECURSE TRUE
  )
  set(BUILD_CTL OFF CACHE BOOL "" FORCE)
  set(BUILD_ORM OFF CACHE BOOL "" FORCE)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_CTL OFF CACHE BOOL "" FORCE)
  set(BUILD_ORM OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(drogon)
endif()

if(AERONET_BENCH_ENABLE_HTTPLIB)
  FetchContent_Declare(
    cpp_httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.26.0.tar.gz
    URL_HASH SHA256=a66f908f50ccb119769adce44fe1eac75f81b6ffab7c4ac0211bb663ffeb2688
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(cpp_httplib)
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
## Internal microbenchmarks: build each source as its own executable
# Keep individual files separate because each defines its own benchmark main.
# If you add more files here, add a corresponding AeronetAddProjectBenchmark call.

set(AERONET_BENCH_INTERNAL_REQUEST_PARSE ${AERONET_BENCH_ROOT}/internal/request-parse_bench.cpp)
set(AERONET_BENCH_INTERNAL_STRING_EQUAL ${AERONET_BENCH_ROOT}/internal/string-equal-ignore-case_bench.cpp)
set(AERONET_BENCH_INTERNAL_ROUTER ${AERONET_BENCH_ROOT}/internal/router_bench.cpp)

include(CheckIPOSupported)

check_ipo_supported(RESULT result OUTPUT output LANGUAGES CXX)
if(result)
  set(AERONET_IPO_SUPPORTED TRUE)
else()
  set(AERONET_IPO_SUPPORTED FALSE)
  message(WARNING "IPO is not supported: ${output}")
endif()

# Helper to create a benchmark executable with common properties.
# Usage: AeronetAddProjectBenchmark(<target> <sources...>)
function(AeronetAddProjectBenchmark target)
  if(ARGC LESS 2)
    message(FATAL_ERROR "AeronetAddProjectBenchmark requires at least target and one source")
  endif()
  # Collect sources (all remaining args)
  set(sources ${ARGN})

  # Create the executable using the project's helper macro
  AeronetAddProjectExecutable(${target} ${sources})
  target_link_libraries(${target} PRIVATE aeronet aeronet_test_support benchmark::benchmark)
  set_target_properties(${target} PROPERTIES FOLDER "benchmarks")

  # Enable LTO/IPO if available and in Release builds
  if(AERONET_IPO_SUPPORTED AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "Activate LTO for ${target}")
  endif()
endfunction()


AeronetAddProjectBenchmark(aeronet-bench-internal-request-parse ${AERONET_BENCH_INTERNAL_REQUEST_PARSE})
set_target_properties(aeronet-bench-internal-request-parse PROPERTIES FOLDER "benchmarks/internal")

AeronetAddProjectBenchmark(aeronet-bench-internal-string-equal-ignore-case ${AERONET_BENCH_INTERNAL_STRING_EQUAL})
set_target_properties(aeronet-bench-internal-string-equal-ignore-case PROPERTIES FOLDER "benchmarks/internal")

AeronetAddProjectBenchmark(aeronet-bench-internal-router ${AERONET_BENCH_INTERNAL_ROUTER})
set_target_properties(aeronet-bench-internal-router PROPERTIES FOLDER "benchmarks/internal")

# Throughput benchmark (simple skeleton; not using Google Benchmark intentionally)
AeronetAddProjectBenchmark(aeronet-bench-throughput ${AERONET_BENCH_ROOT}/e2e/bench_throughput_local.cpp)

# Comparative framework benchmark (aeronet vs optional drogon/oatpp)
set(AERONET_BENCH_FRAMEWORKS_SOURCES ${AERONET_BENCH_ROOT}/frameworks/bench_frameworks_basic.cpp)
AeronetAddProjectBenchmark(aeronet-bench-frameworks ${AERONET_BENCH_FRAMEWORKS_SOURCES})
target_include_directories(aeronet-bench-frameworks PRIVATE ${CMAKE_SOURCE_DIR}/tests)

if(AERONET_BENCH_ENABLE_HTTPLIB AND DEFINED cpp_httplib_SOURCE_DIR)
  target_include_directories(aeronet-bench-frameworks PRIVATE ${cpp_httplib_SOURCE_DIR})
  target_compile_definitions(aeronet-bench-frameworks PRIVATE AERONET_BENCH_ENABLE_HTTPLIB)
endif()

if(AERONET_BENCH_ENABLE_DROGON AND TARGET drogon)
  message(STATUS "[aeronet][bench] Including drogon in framework benchmarks")
  target_link_libraries(aeronet-bench-frameworks PRIVATE drogon)
  target_compile_definitions(aeronet-bench-frameworks PRIVATE AERONET_BENCH_ENABLE_DROGON)
endif()

if(AERONET_BENCH_ENABLE_OATPP)
  # Support either plain 'oatpp' or namespace target 'oatpp::oatpp' depending on version.
  set(_AERONET_OATPP_TARGET "")
  if(TARGET oatpp::oatpp)
    set(_AERONET_OATPP_TARGET oatpp::oatpp)
  elseif(TARGET oatpp)
    set(_AERONET_OATPP_TARGET oatpp)
  endif()
  if(_AERONET_OATPP_TARGET)
    message(STATUS "[aeronet][bench] Including oatpp in framework benchmarks using target '${_AERONET_OATPP_TARGET}'")
    target_link_libraries(aeronet-bench-frameworks PRIVATE ${_AERONET_OATPP_TARGET})
    target_compile_definitions(aeronet-bench-frameworks PRIVATE AERONET_BENCH_ENABLE_OATPP)
    # Some releases may not export the 'src' directory as an include path; add it if available.
    if(DEFINED oatpp_SOURCE_DIR AND EXISTS "${oatpp_SOURCE_DIR}/src/oatpp/web/server/HttpRouter.hpp")
      target_include_directories(aeronet-bench-frameworks PRIVATE ${oatpp_SOURCE_DIR}/src)
    endif()
  else()
    message(WARNING "[aeronet][bench] Oatpp target not found after FetchContent; Oatpp benchmarks disabled")
  endif()
endif()

set_target_properties(aeronet-bench-frameworks PROPERTIES FOLDER "benchmarks")

# Convenience run targets
add_custom_target(run-aeronet-bench
  COMMAND aeronet-bench-internal-request-parse --benchmark_report_aggregates_only=true
  COMMAND aeronet-bench-internal-string-equal-ignore-case --benchmark_report_aggregates_only=true
  COMMAND aeronet-bench-internal-router --benchmark_report_aggregates_only=true
  DEPENDS aeronet-bench-internal-request-parse aeronet-bench-internal-string-equal-ignore-case aeronet-bench-internal-router
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
  COMMAND aeronet-bench-internal-request-parse --benchmark_format=json > aeronet-benchmarks.json
  DEPENDS aeronet-bench-internal-request-parse
  COMMENT "Running aeronet benchmarks (JSON output -> aeronet-benchmarks.json)")

# Scripted server benchmarks (wrk-based external load testing)
# These are standalone HTTP servers meant to be tested with wrk/lua scripts
add_subdirectory(${AERONET_BENCH_ROOT}/scripted-servers ${CMAKE_BINARY_DIR}/benchmarks/scripted-servers)
