cmake_minimum_required(VERSION 3.28)

project(aeronet VERSION 0.1.0 LANGUAGES CXX)

# Options
option(AERONET_BUILD_EXAMPLES "Build example programs" ON)
option(AERONET_BUILD_TESTS "Build tests" ON)
option(AERONET_ENABLE_ASAN "Enable Address/Undefined Behavior Sanitizers" OFF)
option(AERONET_ENABLE_CLANG_TIDY "Enable clang-tidy analysis" OFF)
option(AERONET_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(AERONET_BUILD_SHARED "Build Aeronet libraries as shared instead of static" OFF)
option(AERONET_INSTALL "Enable install & package config generation" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)

include(cmake/CommonSettings.cmake)
include(cmake/AddUnitTest.cmake)

# External dependencies (linked with FetchContent)
include(FetchContent)

set(fetchContentPackagesToMakeAvailable "")

find_package(amc CONFIG)
if(NOT amc_FOUND)
  FetchContent_Declare(
    amadeusamc
    URL https://github.com/AmadeusITGroup/amc/archive/refs/tags/v2.6.0.tar.gz
    URL_HASH SHA256=381cda7834c73cd81f51693739eb3132cfb822a41499972d98621d914924de5f
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
