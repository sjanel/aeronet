cmake_minimum_required(VERSION 3.22)

# Silence FetchContent CMP0135 warnings about archive timestamps by opting in
# to the NEW behavior when supported (policy introduced in newer CMake).
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# Centralize version: read from VERSION file at repo root (fallback to 0.0.0 if missing)
set(AERONET_PROJECT_VERSION "0.0.0")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AERONET_VERSION_FILE)
  string(STRIP "${AERONET_VERSION_FILE}" AERONET_PROJECT_VERSION)
endif()
project(aeronet VERSION ${AERONET_PROJECT_VERSION} LANGUAGES CXX)

message(STATUS "[${CMAKE_PROJECT_NAME}] version: ${AERONET_PROJECT_VERSION}")

#
# MAIN_PROJECT CHECK
# determine if coincenter is built as a sub-project or if it is the main project
#
set(MAIN_PROJECT OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(MAIN_PROJECT ON)
endif()

set(ASAN_DEFAULT OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(ASAN_DEFAULT ON)
endif()

option(AERONET_ENABLE_CCACHE "Compile with ccache for faster incremental compilation" ${MAIN_PROJECT})

# Build options
option(AERONET_INSTALL "Enable install & package config generation" ${MAIN_PROJECT})
# When consumed as a package (not the main project) we never want to build tests or examples.
# Some package managers (vcpkg/Conan) may still pass defaults or reuse a cached build tree;
# enforce OFF deterministically here to avoid leaking test targets into consumer builds.
set(AERONET_BUILD_TESTS_EXAMPLES_DEFAULT ${MAIN_PROJECT})
if(AERONET_INSTALL AND NOT MAIN_PROJECT)
  set(AERONET_BUILD_TESTS_EXAMPLES_DEFAULT OFF CACHE BOOL "Build tests and examples" FORCE)
endif()

option(AERONET_BUILD_EXAMPLES "Build example programs" ${AERONET_BUILD_TESTS_EXAMPLES_DEFAULT})
option(AERONET_BUILD_TESTS "Build tests" ${AERONET_BUILD_TESTS_EXAMPLES_DEFAULT})
option(AERONET_BUILD_SHARED "Build Aeronet libraries as shared instead of static" OFF)
option(AERONET_BUILD_BENCHMARKS "Build benchmark executables" ${AERONET_BUILD_TESTS_EXAMPLES_DEFAULT})

# Features
# Optional dependencies (aeronet is able to compile without the feature if not enabled)
option(AERONET_ENABLE_SPDLOG "Enable usage of spdlog library for logging" ${MAIN_PROJECT})
option(AERONET_ENABLE_OPENSSL "Enable usage of OpenSSL for HTTPS support" ${MAIN_PROJECT})
option(AERONET_ENABLE_ZLIB "Enable usage of zlib for HTTP response compression" ${MAIN_PROJECT})

# Linting
option(AERONET_ENABLE_ASAN "Enable Address/Undefined Behavior Sanitizers" ${ASAN_DEFAULT})
option(AERONET_ENABLE_CLANG_TIDY "Enable clang-tidy analysis" OFF)
option(AERONET_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

if(AERONET_ENABLE_CCACHE)
  # Use ccache if available
  find_program(CCACHE_PROGRAM ccache)

  if(CCACHE_PROGRAM)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
  else()
    message(WARNING "ccache wanted but not found in PATH")
  endif()
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Clang-Tidy integration
if(AERONET_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY "clang-tidy")
  if(CLANG_TIDY)
    message(STATUS "Activate clang-tidy")
  else()
    message(FATAL_ERROR "clang-tidy executable cannot be found")
  endif()
endif()

if(AERONET_ENABLE_ASAN)
  if (NOT AERONET_ASAN_OPTIONS)
    set(AERONET_ASAN_OPTIONS -fsanitize=address -fsanitize=undefined -fsanitize=float-divide-by-zero -fno-sanitize-recover)
  endif()
  message(STATUS "Activate asan with options ${AERONET_ASAN_OPTIONS}")
endif()

include(cmake/CommonSettings.cmake)
include(cmake/AddUnitTest.cmake)

# External dependencies (linked with FetchContent)
include(FetchContent)

set(fetchContentPackagesToMakeAvailable "")

set(LINK_AMC FALSE)
find_package(amc CONFIG)
if(amc_FOUND)
  # amc provided by toolchain / package manager
  set(LINK_AMC TRUE)
else()
  # Fallback: attempt to fetch the dependency (unless disconnected prevents it)
  set(LINK_AMC TRUE)
  FetchContent_Declare(
    amadeusamc
    URL https://github.com/AmadeusITGroup/amc/archive/refs/tags/v2.6.1.tar.gz
    URL_HASH SHA256=a1cbb695f31c96b90699ef1d2db14dcd89984cf1135c29b48af70a007c44a02d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  list(APPEND fetchContentPackagesToMakeAvailable amadeusamc)
endif()

if(AERONET_BUILD_TESTS)
  find_package(GTest CONFIG)

  if(NOT GTest_FOUND)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
      URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    # Use modern position independent etc.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

    list(APPEND fetchContentPackagesToMakeAvailable googletest)
  endif()

  enable_testing()
endif()

if(AERONET_ENABLE_SPDLOG)
    # spdlog - logging library
    find_package(spdlog CONFIG)
    if(NOT spdlog_FOUND)
        FetchContent_Declare(
            spdlog
            URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.3.tar.gz
            URL_HASH SHA256=15a04e69c222eb6c01094b5c7ff8a249b36bb22788d72519646fb85feb267e67
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )

        set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)

        list(APPEND fetchContentPackagesToMakeAvailable spdlog)
    endif()
endif()

# Make fetch content available
if(fetchContentPackagesToMakeAvailable)
  message(STATUS "Configuring packages ${fetchContentPackagesToMakeAvailable}")
  # In vcpkg builds, FETCHCONTENT_FULLY_DISCONNECTED is forced ON, so
  # population of new content that is not already present will fail.
  # We let FetchContent attempt population; if it fails to create the
  # expected targets we degrade gracefully by disabling AMC linkage.
  FetchContent_MakeAvailable("${fetchContentPackagesToMakeAvailable}")
endif()

# AMC dependency handling
# If we intend to link AMC but no target materialized, decide whether we can degrade.
if(LINK_AMC AND NOT TARGET amc::amc)
  if(FETCHCONTENT_FULLY_DISCONNECTED)
    message(WARNING "[aeronet] amc dependency unavailable under disconnected mode; disabling AMC integration for this build")
    set(LINK_AMC FALSE)
  else()
    message(FATAL_ERROR "[aeronet] Required dependency 'amc' not found and not in disconnected mode. Please provide amc via find_package(amc CONFIG) or allow FetchContent to download it.")
  endif()
endif()

if(AERONET_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Compression libraries (optional, isolated like TLS). Detect BEFORE building core targets so
# compile definitions propagate correctly to aeronet library sources.
if(AERONET_ENABLE_ZLIB)
  find_package(ZLIB)
  if(NOT ZLIB_FOUND)
    message(WARNING "[aeronet] AERONET_ENABLE_ZLIB=ON but zlib not found; compression will be disabled.")
    set(AERONET_ENABLE_ZLIB OFF CACHE BOOL "Enable usage of zlib for HTTP response compression" FORCE)
  else()
    message(STATUS "[aeronet] zlib found: version=${ZLIB_VERSION}")
  endif()
endif()

add_subdirectory(aeronet)

# Optional TLS module kept separate to minimize #ifdef sprawl.
if(AERONET_ENABLE_OPENSSL)  
  add_subdirectory(aeronet/tls)
endif()

if(AERONET_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(AERONET_BUILD_BENCHMARKS)
  include(cmake/Benchmarks.cmake)
endif()

# Installation & Packaging
if(AERONET_INSTALL)
    include(CMakePackageConfigHelpers)
    set(AERONET_INSTALL_CMAKE_DIR "lib/cmake/aeronet")

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/aeronetConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/aeronetConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/aeronetConfig.cmake
        @ONLY
    )

  install(TARGETS aeronet aeronet_http aeronet_tech aeronet_objects aeronet_sys
    EXPORT aeronetTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
  )
  if(AERONET_ENABLE_OPENSSL)
    install(TARGETS aeronet_tls
      EXPORT aeronetTargets
      ARCHIVE DESTINATION lib
      LIBRARY DESTINATION lib
      RUNTIME DESTINATION bin
      INCLUDES DESTINATION include
    )
  endif()

    # Headers: install each public include tree
    install(DIRECTORY aeronet/main/include/ DESTINATION include)
    install(DIRECTORY aeronet/http/include/ DESTINATION include)
    install(DIRECTORY aeronet/tech/include/ DESTINATION include)
    install(DIRECTORY aeronet/sys/include/ DESTINATION include)

    install(DIRECTORY aeronet/objects/include/ DESTINATION include)
    if(AERONET_ENABLE_OPENSSL)
      install(DIRECTORY aeronet/tls/include/ DESTINATION include)
    endif()

    install(EXPORT aeronetTargets
        NAMESPACE aeronet::
        FILE aeronetTargets.cmake
        DESTINATION ${AERONET_INSTALL_CMAKE_DIR}
    )

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/aeronetConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/aeronetConfigVersion.cmake"
        DESTINATION ${AERONET_INSTALL_CMAKE_DIR}
    )
endif()
