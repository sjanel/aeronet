cmake_minimum_required(VERSION 3.28)

# Newer behaviors when CMake knows them: CMP0135 (FetchContent archive timestamps) and CMP0156 / CMP0179 (drop the
# duplicated libraries of the link lines, which our mutually dependent static libraries would otherwise repeat, and
# which the Apple linker warns about; the project link order is kept).
foreach(_policy IN ITEMS CMP0135 CMP0156 CMP0179)
  if(POLICY ${_policy})
    cmake_policy(SET ${_policy} NEW)
  endif()
endforeach()

include(cmake/ProjectVersion.cmake)
AeronetReadVersion(AERONET_VERSION_NUMBER AERONET_VERSION_SUFFIX)

project(aeronet VERSION ${AERONET_VERSION_NUMBER} LANGUAGES CXX)

# Full version: numeric part and optional pre-release suffix.
set(AERONET_PROJECT_VERSION "${PROJECT_VERSION}")
if(AERONET_VERSION_SUFFIX)
  string(APPEND AERONET_PROJECT_VERSION "-${AERONET_VERSION_SUFFIX}")
endif()
message(STATUS "[aeronet] version: ${AERONET_PROJECT_VERSION}")

# 64-bit targets only (CMAKE_SIZEOF_VOID_P is empty for a multi-architecture macOS build).
if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  math(EXPR _target_bits "${CMAKE_SIZEOF_VOID_P} * 8")
  message(FATAL_ERROR
    "[aeronet] ${_target_bits}-bit targets are not supported: aeronet requires a 64-bit target (x86_64, aarch64...). "
    "See docs/getting-started/install.md#64-bit-targets-only")
endif()

# ----------------------------------------------------------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------------------------------------------------------

# Build
option(AERONET_ENABLE_CCACHE "Compile with ccache for faster incremental compilation" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_INSTALL "Enable install & package config generation" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_BUILD_SHARED "Build aeronet libraries as shared instead of static" OFF)
option(AERONET_BUILD_MODULES "Build aeronet libraries as C++ module (experimental)" OFF)
option(AERONET_UNITY_BUILD "Build the aeronet libraries as unity builds (several sources per compilation)" OFF)

# Tests, examples and benchmarks are only built by default as the main project (benchmarks: not in Debug).
set(AERONET_BUILD_BENCHMARKS_DEFAULT ${PROJECT_IS_TOP_LEVEL})
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(AERONET_BUILD_BENCHMARKS_DEFAULT OFF)
endif()
option(AERONET_BUILD_EXAMPLES "Build example programs" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_BUILD_BENCHMARKS "Build benchmark executables" ${AERONET_BUILD_BENCHMARKS_DEFAULT})

set(AERONET_ENABLE_TEST_HOOKS_DEFAULT OFF)
if(AERONET_BUILD_TESTS AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  set(AERONET_ENABLE_TEST_HOOKS_DEFAULT ON)
endif()
option(AERONET_ENABLE_TEST_HOOKS
  "Enable transport test hooks (auto-on in non-Release test builds, off by default for Release)"
  ${AERONET_ENABLE_TEST_HOOKS_DEFAULT})

# Installed artifacts would reference the .dwo files left in the build tree: only on by default for Debug test builds.
set(AERONET_COMPACT_DEBUG_INFO_DEFAULT OFF)
if(AERONET_BUILD_TESTS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(AERONET_COMPACT_DEBUG_INFO_DEFAULT ON)
endif()
option(AERONET_COMPACT_DEBUG_INFO
  "Split DWARF + compressed debug sections, to shrink the binaries (on by default for Debug test builds)"
  ${AERONET_COMPACT_DEBUG_INFO_DEFAULT})

# Features: all optional and independent of each other (WebSocket without zlib simply has no permessage-deflate).
option(AERONET_ENABLE_HTTP_CLIENT "Enable the HTTP client (HttpClient)" ON)
option(AERONET_ENABLE_HTTP_SERVER "Enable the HTTP server (HttpServer)" ON)

# Features without external dependencies: enabled by default, disable them to reduce binary size and compilation time.
option(AERONET_ENABLE_ASYNC_HANDLERS "Enable asynchronous routing handlers" ${AERONET_ENABLE_HTTP_SERVER})
option(AERONET_ENABLE_HTTP2 "Enable HTTP/2 (RFC 9113) support" ON)
option(AERONET_ENABLE_WEBSOCKET "Enable WebSocket support" ${AERONET_ENABLE_HTTP_SERVER})

# Features with external dependencies: disabled by default, except as the main project.
option(AERONET_ENABLE_SPDLOG "Use spdlog library for logging" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_ENABLE_OPENSSL "Enable usage of OpenSSL for HTTPS support" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_ENABLE_BROTLI "Enable usage of Brotli for br codec support" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_ENABLE_ZLIB "Enable usage of zlib for deflate and gzip codec support" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_ENABLE_ZSTD "Enable usage of Zstandard for zstd codec support" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_ENABLE_OPENTELEMETRY "Enable OpenTelemetry C++ integration" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_ENABLE_GLAZE "Enable glaze library for JSON serialization" ${PROJECT_IS_TOP_LEVEL})

# JWT (JWS) reuses the OpenSSL crypto and the glaze JSON parser, and adds no dependency: on whenever both are, else off.
include(CMakeDependentOption)
cmake_dependent_option(AERONET_ENABLE_JWT "Enable JWT (JWS) support"
    ON "AERONET_ENABLE_OPENSSL;AERONET_ENABLE_GLAZE" OFF)

# Dependency variants
option(AERONET_ENABLE_ZLIBNG "Prefer zlib-ng over zlib when AERONET_ENABLE_ZLIB is ON" ON)
option(AERONET_SPDLOG_USE_STD_FORMAT "Use std::format instead of fmt::format in spdlog" ON)

# Developer tools
option(AERONET_ENABLE_ASAN "Enable Address/Undefined Behavior Sanitizers" OFF)
option(AERONET_ENABLE_TSAN "Enable Thread Sanitizer (cannot be combined with AERONET_ENABLE_ASAN)" OFF)
option(AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS "Enable miscellaneous additional memory checks to complement ASAN" OFF)
option(AERONET_ENABLE_CLANG_TIDY "Enable clang-tidy analysis" OFF)
option(AERONET_ENABLE_WARNINGS "Enable extra compiler warnings" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

if(NOT AERONET_ENABLE_HTTP_CLIENT AND NOT AERONET_ENABLE_HTTP_SERVER)
  message(FATAL_ERROR "[aeronet] invalid configuration - client and server cannot be both disabled")
endif()
if(AERONET_ENABLE_ASAN AND AERONET_ENABLE_TSAN)
  message(FATAL_ERROR "AERONET_ENABLE_ASAN and AERONET_ENABLE_TSAN cannot be enabled together")
endif()

# ----------------------------------------------------------------------------------------------------------------------
# Toolchain and dependencies
# ----------------------------------------------------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(AERONET_BUILD_SHARED)
  # Static dependencies (zlib-ng, zstd...) are linked into the aeronet shared libraries.
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  # Editing the implementation of a shared library only relinks that library, not the tests and examples.
  set(CMAKE_LINK_DEPENDS_NO_SHARED ON)
endif()

# CMake >= 3.28 scans every C++ source for module imports by default. Only aeronet_module needs it (it enables it
# itself, see modules/CMakeLists.txt): off for everything else, FetchContent dependencies included. On MSVC + Ninja
# outside a VS Developer Command Prompt, the scanner also runs without system include paths and fails.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(cmake/MsvcSystemPaths.cmake)
AeronetInjectMsvcSystemPaths()

include(cmake/BuildTooling.cmake)
# Before the dependencies, which are linked into the test executables as well.
include(cmake/CompactDebugInfo.cmake)
include(cmake/Dependencies.cmake)
include(cmake/JobPool.cmake)
include(cmake/SetProjectProperties.cmake)
include(cmake/AddUnitTest.cmake)

# ----------------------------------------------------------------------------------------------------------------------
# Targets
# ----------------------------------------------------------------------------------------------------------------------

add_subdirectory(aeronet)

if(AERONET_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# Tests need the server.
if(AERONET_BUILD_TESTS AND AERONET_ENABLE_HTTP_SERVER)
  add_subdirectory(tests)
endif()

if(AERONET_BUILD_MODULES)
  add_subdirectory(modules)
endif()

if(AERONET_BUILD_BENCHMARKS)
  include(cmake/Benchmarks.cmake)
endif()

# Headers are not translation units: CXX_CLANG_TIDY never analyzes them.
if(AERONET_ENABLE_CLANG_TIDY)
  include(cmake/ClangTidyHeaders.cmake)
endif()

if(AERONET_INSTALL)
  include(cmake/Install.cmake)
endif()
