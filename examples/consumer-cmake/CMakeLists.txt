cmake_minimum_required(VERSION 3.22)

project(aeronet-consumer-example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

# Demonstrates consuming an installed/config-packaged aeronet library.
# Assumes aeronet was installed and a find_package(aeronet CONFIG REQUIRED) is available.

find_package(aeronet CONFIG REQUIRED)

add_executable(consumer-example ../minimal.cpp)

# Link against the imported target installed by aeronet
# (public headers and transitive usage requirements are propagated)
target_link_libraries(consumer-example PRIVATE aeronet::aeronet)
