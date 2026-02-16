cmake_minimum_required(VERSION 3.28)
project(aeronet-techempower-benchmark CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT AERONET_ENABLE_GLAZE)
    message(STATUS "Skipping TechEmpower benchmark (requires AERONET_ENABLE_GLAZE=ON)")
    return()
endif()

# Build the benchmark executable
if(COMMAND AeronetAddProjectExecutable)
    AeronetAddProjectExecutable(benchmark_techempower src/benchmark.cpp)
else()
    add_executable(benchmark_techempower src/benchmark.cpp)
endif()

# In-tree build (this repository): target already exists.
# Standalone build (installed package): resolve with find_package.
if(TARGET aeronet::aeronet)
    target_link_libraries(benchmark_techempower PRIVATE aeronet::aeronet)
elseif(TARGET aeronet)
    target_link_libraries(benchmark_techempower PRIVATE aeronet)
else()
    find_package(aeronet REQUIRED CONFIG)
    target_link_libraries(benchmark_techempower PRIVATE aeronet::aeronet)
endif()

message(STATUS "Building TechEmpower benchmark with JSON support (glaze enabled)")
