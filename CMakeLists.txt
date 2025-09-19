cmake_minimum_required(VERSION 3.28)

project(aeronet VERSION 0.1.0 LANGUAGES CXX)

# Options
option(AERONET_BUILD_EXAMPLES "Build example programs" ON)
option(AERONET_BUILD_TESTS "Build tests" ON)
option(AERONET_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Basic warnings
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if (AERONET_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

find_package(OpenSSL REQUIRED)

add_subdirectory(aeronet) # new nested library location

if(AERONET_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(AERONET_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    )
    # Use modern position independent etc.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    add_subdirectory(tests)
endif()
