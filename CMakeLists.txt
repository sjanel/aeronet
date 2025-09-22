cmake_minimum_required(VERSION 3.28)

project(aeronet VERSION 0.1.0 LANGUAGES CXX)

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

# Build options
option(AERONET_BUILD_EXAMPLES "Build example programs" ${MAIN_PROJECT})
option(AERONET_BUILD_TESTS "Build tests" ${MAIN_PROJECT})
option(AERONET_BUILD_SHARED "Build Aeronet libraries as shared instead of static" OFF)
option(AERONET_INSTALL "Enable install & package config generation" ${MAIN_PROJECT})

# Optional dependencies (aeronet is able to compile without the feature if not enabled)
option(AERONET_ENABLE_SPDLOG "Enable usage of spdlog library for logging" OFF)
option(AERONET_ENABLE_OPENSSL "Enable usage of OpenSSL for HTTPS support" OFF)

# Linting
option(AERONET_ENABLE_ASAN "Enable Address/Undefined Behavior Sanitizers" ${ASAN_DEFAULT})
option(AERONET_ENABLE_CLANG_TIDY "Enable clang-tidy analysis" OFF)
option(AERONET_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(AERONET_ENABLE_OPENSSL)
  find_package(OpenSSL REQUIRED)
endif()

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

find_package(amc CONFIG)
if(amc_FOUND)
  set(LINK_AMC FALSE)
else()
  set(LINK_AMC TRUE)
  FetchContent_Declare(
    amadeusamc
    URL https://github.com/AmadeusITGroup/amc/archive/refs/tags/v2.6.1.tar.gz
    URL_HASH SHA256=a1cbb695f31c96b90699ef1d2db14dcd89984cf1135c29b48af70a007c44a02d
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
        )

        set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)

        list(APPEND fetchContentPackagesToMakeAvailable spdlog)
    endif()
endif()

# Make fetch content available
if(fetchContentPackagesToMakeAvailable)
  message(STATUS "Configuring packages ${fetchContentPackagesToMakeAvailable}")

  FetchContent_MakeAvailable("${fetchContentPackagesToMakeAvailable}")
endif()

if(AERONET_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

add_subdirectory(aeronet)

if(AERONET_BUILD_EXAMPLES)
    add_subdirectory(examples)
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

    install(TARGETS aeronet aeronet_http aeronet_tech
        EXPORT aeronetTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
        INCLUDES DESTINATION include
    )

    # Headers: install each public include tree
    install(DIRECTORY aeronet/main/include/ DESTINATION include)
    install(DIRECTORY aeronet/http/include/ DESTINATION include)
    install(DIRECTORY aeronet/tech/include/ DESTINATION include)

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
