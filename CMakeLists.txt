cmake_minimum_required(VERSION 3.28)

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
else()
  message(WARNING "VERSION file not found; defaulting to version ${AERONET_PROJECT_VERSION}")
endif()
project(aeronet VERSION ${AERONET_PROJECT_VERSION} LANGUAGES CXX)
set(AERONET_PROJECT_VERSION ${PROJECT_VERSION} CACHE INTERNAL "aeronet project version")

# ---------------------------------------------------------------------------
# MSVC system path injection (Ninja + MSVC without VS Developer Command Prompt)
# ---------------------------------------------------------------------------
# When CMake is configured with the Ninja generator + MSVC outside a VS
# Developer Command Prompt, both CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES and
# CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES are empty.  This causes:
#   - LNK1104 for bare library names (ws2_32, mswsock, …)
#   - C1083 "Cannot open include file: 'string'" for standard headers
# We detect the MSVC toolchain root from CMAKE_CXX_COMPILER and the Windows
# SDK root from the registry, then inject both the include and lib dirs.
if(WIN32 AND MSVC AND NOT CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
  # --- MSVC include + lib dirs ------------------------------------------------
  # CMAKE_CXX_COMPILER = .../VC/Tools/MSVC/<ver>/bin/Hostx64/x64/cl.exe
  # so parent of parent of parent of parent = .../VC/Tools/MSVC/<ver>
  get_filename_component(_cl_dir  "${CMAKE_CXX_COMPILER}"  DIRECTORY)  # .../Hostx64/x64
  get_filename_component(_cl_dir  "${_cl_dir}"             DIRECTORY)  # .../Hostx64
  get_filename_component(_cl_dir  "${_cl_dir}"             DIRECTORY)  # .../bin
  get_filename_component(_msvc_root "${_cl_dir}"           DIRECTORY)  # .../MSVC/<ver>
  set(_msvc_include "${_msvc_root}/include")
  if(EXISTS "${_msvc_include}")
    message(STATUS "[aeronet] Injecting MSVC include dir (no VS env): ${_msvc_include}")
    include_directories(SYSTEM "${_msvc_include}")
  endif()
  # MSVC lib dir: .../MSVC/<ver>/lib/x64 — needed for msvcprt, vcruntime, etc.
  set(_msvc_libdir "${_msvc_root}/lib/x64")
  if(EXISTS "${_msvc_libdir}")
    message(STATUS "[aeronet] Injecting MSVC lib dir (no VS env): ${_msvc_libdir}")
    link_directories("${_msvc_libdir}")
  endif()
  unset(_cl_dir)
  unset(_msvc_root)
  unset(_msvc_include)
  unset(_msvc_libdir)

  # --- Windows SDK include + lib dirs -----------------------------------------
  cmake_host_system_information(
    RESULT _winsdk_root
    QUERY WINDOWS_REGISTRY
      "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
    VALUE "KitsRoot10"
    VIEW HOST
  )
  if(_winsdk_root)
    cmake_path(SET _winsdk_root NORMALIZE "${_winsdk_root}")

    # Newest SDK version directory under Include/
    file(GLOB _winsdk_inc_dirs "${_winsdk_root}/Include/*")
    list(SORT _winsdk_inc_dirs ORDER DESCENDING)
    if(_winsdk_inc_dirs)
      list(GET _winsdk_inc_dirs 0 _winsdk_inc_ver)
      foreach(_sub ucrt shared um winrt km)
        if(EXISTS "${_winsdk_inc_ver}/${_sub}")
          message(STATUS "[aeronet] Injecting WinSDK include (no VS env): ${_winsdk_inc_ver}/${_sub}")
          include_directories(SYSTEM "${_winsdk_inc_ver}/${_sub}")
        endif()
      endforeach()
    endif()

    # Newest SDK version directory under Lib/
    file(GLOB _winsdk_um_dirs "${_winsdk_root}/Lib/*/um/x64")
    list(SORT _winsdk_um_dirs ORDER DESCENDING)
    if(_winsdk_um_dirs)
      list(GET _winsdk_um_dirs 0 _winsdk_libdir)
      message(STATUS "[aeronet] Injecting Windows SDK lib dir (no VS env): ${_winsdk_libdir}")
      link_directories("${_winsdk_libdir}")
      # ucrt/x64 is needed for ucrtd.lib / ucrt.lib (Universal CRT)
      get_filename_component(_winsdk_lib_ver "${_winsdk_libdir}" DIRECTORY)  # .../um
      get_filename_component(_winsdk_lib_ver "${_winsdk_lib_ver}" DIRECTORY) # .../10.0.26100.0
      set(_winsdk_ucrt_lib "${_winsdk_lib_ver}/ucrt/x64")
      if(EXISTS "${_winsdk_ucrt_lib}")
        message(STATUS "[aeronet] Injecting WinSDK ucrt lib dir (no VS env): ${_winsdk_ucrt_lib}")
        link_directories("${_winsdk_ucrt_lib}")
      endif()
    endif()
  endif()
  unset(_winsdk_root)
  unset(_winsdk_inc_dirs)
  unset(_winsdk_inc_ver)
  unset(_winsdk_um_dirs)
  unset(_winsdk_libdir)
endif()
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# CMake 4.x enables C++ module dependency scanning by default for C++20/23.
# With MSVC + Ninja outside a VS Developer Command Prompt the scanner runs
# without the MSVC system include paths and fails on any header that pulls in
# <string>, <stdint.h>, etc. (OpenSSL, protobuf, etc.).  Since this project
# does not use C++ named modules, disable scanning globally for MSVC builds.
if(MSVC)
  set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif()
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

message(STATUS "[aeronet] version: ${AERONET_PROJECT_VERSION}")

set(AERONET_BUILD_BENCHMARKS_DEFAULT ${PROJECT_IS_TOP_LEVEL})
set(ASAN_DEFAULT OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  if(NOT WIN32)
    set(ASAN_DEFAULT ON)
  endif()
  set(AERONET_BUILD_BENCHMARKS_DEFAULT OFF)
endif()

option(AERONET_ENABLE_CCACHE "Compile with ccache for faster incremental compilation" ${PROJECT_IS_TOP_LEVEL})

# Build options
option(AERONET_INSTALL "Enable install & package config generation" ${PROJECT_IS_TOP_LEVEL})
# When consumed as a package (not the main project) we never want to build tests or examples.
# Some package managers (vcpkg/Conan) may still pass defaults or reuse a cached build tree;
# enforce OFF deterministically here to avoid leaking test targets into consumer builds.
set(AERONET_BUILD_TESTS_EXAMPLES_DEFAULT ${PROJECT_IS_TOP_LEVEL})
if(AERONET_INSTALL AND NOT PROJECT_IS_TOP_LEVEL)
  set(AERONET_BUILD_TESTS_EXAMPLES_DEFAULT OFF CACHE BOOL "Build tests and examples" FORCE)
  set(AERONET_BUILD_BENCHMARKS_DEFAULT OFF CACHE BOOL "Build benchmarks" FORCE)
endif()

option(AERONET_BUILD_EXAMPLES "Build example programs" ${AERONET_BUILD_TESTS_EXAMPLES_DEFAULT})
option(AERONET_BUILD_TESTS "Build tests" ${AERONET_BUILD_TESTS_EXAMPLES_DEFAULT})
option(AERONET_BUILD_SHARED "Build aeronet libraries as shared instead of static" OFF)
option(AERONET_BUILD_MODULES "Build aeronet libraries as C++ module (experimental)" OFF)
option(AERONET_BUILD_BENCHMARKS "Build benchmark executables" ${AERONET_BUILD_BENCHMARKS_DEFAULT})

# Features
# All features are optional, and independent of each other.
# For example, you may activate WebSocket without zlib support, in which case the WebSocket implementation will simply not support permessage-deflate compression.

# Features needing external dependencies (disabled by default, except if compiled as main project)
option(AERONET_ENABLE_SPDLOG "Use spdlog library for logging" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_ENABLE_OPENSSL "Enable usage of OpenSSL for HTTPS support" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_ENABLE_BROTLI "Enable usage of Brotli for br codec support" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_ENABLE_ZLIB "Enable usage of zlib for deflate and gzip codec support" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_ENABLE_ZSTD "Enable usage of Zstandard for zstd codec support" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_ENABLE_OPENTELEMETRY "Enable OpenTelemetry C++ integration" ${PROJECT_IS_TOP_LEVEL})
option(AERONET_ENABLE_GLAZE "Enable glaze library for JSON serialization" ${PROJECT_IS_TOP_LEVEL})

# Features without external dependencies (enabled by default, but you can disable them if you don't need them to reduce binary size and compilation time)
option(AERONET_ENABLE_ASYNC_HANDLERS "Enable asynchronous routing handlers" ON)
option(AERONET_ENABLE_HTTP2 "Enable HTTP/2 (RFC 9113) support" ON)
option(AERONET_ENABLE_WEBSOCKET "Enable WebSocket support" ON)

# zlib-ng is a drop-in replacement for zlib with better performance and compression ratios, and is the default when AERONET_ENABLE_ZLIB is ON.
# You can disable it in favor of classic zlib by setting this option to OFF.
option(AERONET_ENABLE_ZLIBNG "Prefer zlib-ng over zlib when AERONET_ENABLE_ZLIB is ON" ON)

# Miscellaneous
option(AERONET_SPDLOG_USE_STD_FORMAT "Use std::format instead of fmt::format in spdlog" ON)

# Linting
option(AERONET_ENABLE_ASAN "Enable Address/Undefined Behavior Sanitizers" ${ASAN_DEFAULT})
option(AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS "Enable miscellaneous additional memory checks to complement ASAN" OFF)
option(AERONET_ENABLE_CLANG_TIDY "Enable clang-tidy analysis" OFF)
option(AERONET_ENABLE_WARNINGS "Enable extra compiler warnings" ${PROJECT_IS_TOP_LEVEL})
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

include(cmake/Dependencies.cmake)

# Job pool detection and defaults (compute cpus and RAM-derived link pool)
include(cmake/JobPool.cmake)

include(cmake/SetProjectProperties.cmake)
include(cmake/AddUnitTest.cmake)

add_subdirectory(aeronet)

if(AERONET_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(AERONET_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(AERONET_BUILD_MODULES)
  add_subdirectory(modules)
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

  set(_AERONET_EXPORT_TARGETS aeronet aeronet_http aeronet_tech aeronet_objects aeronet_sys)
  if(AERONET_ENABLE_HTTP2)
    list(APPEND _AERONET_EXPORT_TARGETS aeronet_http2)
  endif()
  if(AERONET_ENABLE_WEBSOCKET)
    list(APPEND _AERONET_EXPORT_TARGETS aeronet_websocket)
  endif()
  if(TARGET aeronet_zstd_headers)
    list(APPEND _AERONET_EXPORT_TARGETS aeronet_zstd_headers)
  endif()
  if(TARGET aeronet_brotli_headers)
    list(APPEND _AERONET_EXPORT_TARGETS aeronet_brotli_headers)
  endif()
  install(TARGETS ${_AERONET_EXPORT_TARGETS}
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
  if(AERONET_ENABLE_HTTP2)
    install(DIRECTORY aeronet/http2/include/ DESTINATION include)
  endif()
  install(DIRECTORY aeronet/tech/include/ DESTINATION include)
  install(DIRECTORY aeronet/sys/include/ DESTINATION include)

  install(DIRECTORY aeronet/objects/include/ DESTINATION include)
  if(AERONET_ENABLE_OPENSSL)
    install(DIRECTORY aeronet/tls/include/ DESTINATION include)
  endif()

  # Export brotli third-party static libs when enabled so exported aeronet targets have resolvable deps.
  if(AERONET_ENABLE_BROTLI)
    if(TARGET brotlicommon)
      install(TARGETS brotlicommon EXPORT aeronetTargets ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
    endif()
    if(TARGET brotlidec)
      install(TARGETS brotlidec EXPORT aeronetTargets ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
    endif()
    if(TARGET brotlienc)
      install(TARGETS brotlienc EXPORT aeronetTargets ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
    endif()
  endif()

  # Skip exporting aeronetTargets when opentelemetry is built via FetchContent,
  # as the non-exported opentelemetry targets can't be included in the export set.
  # Users should rely on find_package(aeronet) config for system-installed builds
  # or use FetchContent for the full dependency tree including opentelemetry.
  if(NOT (AERONET_ENABLE_OPENTELEMETRY AND DEFINED opentelemetry_cpp_SOURCE_DIR))
    install(EXPORT aeronetTargets
      NAMESPACE aeronet::
      FILE aeronetTargets.cmake
      DESTINATION ${AERONET_INSTALL_CMAKE_DIR}
    )
  endif()

  if(AERONET_ENABLE_WEBSOCKET)
    install(DIRECTORY aeronet/websocket/include/ DESTINATION include)
  endif()

  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/aeronetConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/aeronetConfigVersion.cmake"
    DESTINATION ${AERONET_INSTALL_CMAKE_DIR}
  )
endif()
