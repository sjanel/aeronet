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
  # Prefer an existing package manager supplied spdlog (vcpkg/conan/system) before fetching.
  find_package(spdlog CONFIG)
  if(NOT spdlog_FOUND)
    FetchContent_Declare(
      spdlog
      URL https://github.com/gabime/spdlog/archive/refs/tags/v1.16.0.tar.gz
      URL_HASH SHA256=8741753e488a78dd0d0024c980e1fb5b5c85888447e309d9cb9d949bdb52aa3e
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    list(APPEND fetchContentPackagesToMakeAvailable spdlog)
  endif()
  if (AERONET_SPDLOG_USE_STD_FORMAT)
    set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
  endif()
endif()

# Compression libraries (optional, isolated like TLS). Detect BEFORE building core targets so
# compile definitions propagate correctly to aeronet library sources.
if(AERONET_ENABLE_ZLIB)
  find_package(ZLIB REQUIRED)
endif()

if(AERONET_ENABLE_ZSTD)
  # Try to locate an existing zstd installation (vcpkg, system) first.
  find_package(zstd CONFIG QUIET)
  if(NOT (TARGET zstd::libzstd OR TARGET zstd::zstd OR TARGET ZSTD::ZSTD OR TARGET libzstd_static))
    # Fallback: build from source via FetchContent.
    set(ZSTD_BUILD_STATIC ON CACHE INTERNAL "Only build static lib")
    set(ZSTD_BUILD_SHARED OFF CACHE INTERNAL "Do not build shared lib")
    set(ZSTD_LEGACY_SUPPORT OFF CACHE INTERNAL "No legacy support")
    set(ZSTD_BUILD_PROGRAMS OFF CACHE INTERNAL "Do not build programs")
    set(ZSTD_BUILD_TESTS OFF CACHE INTERNAL "Do not build tests")

    # Suppress upstream warning: ensure BUILD_SHARED_LIBS is OFF while configuring the zstd subproject.
    if(DEFINED BUILD_SHARED_LIBS)
      set(_AERONET_PREV_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
    endif()
    # Force OFF for the duration of zstd population (static linking desired here).
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "(temporarily)" FORCE)

    set(zstd_URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz)
    set(zstd_URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3)

    FetchContent_Declare(
      zstd
      URL "${zstd_URL}"
      URL_HASH "${zstd_URL_HASH}"
      SOURCE_SUBDIR build/cmake
    )
    list(APPEND fetchContentPackagesToMakeAvailable zstd)
    # We'll restore BUILD_SHARED_LIBS after FetchContent_MakeAvailable below.
  endif()
endif()

if(AERONET_ENABLE_BROTLI)
  # Try to locate an existing brotli installation first.
  find_package(PkgConfig QUIET)
  if(NOT TARGET brotlicommon OR NOT TARGET brotlidec OR NOT TARGET brotlienc)
    # Fallback FetchContent of google/brotli (static libs only)
    set(BROTLI_DISABLE_TESTS ON CACHE INTERNAL "Disable brotli tests")
    # Hash updated to match upstream v1.1.0 release archive.
    FetchContent_Declare(
      brotli
      URL https://github.com/google/brotli/archive/refs/tags/v1.1.0.tar.gz
      URL_HASH SHA256=e720a6ca29428b803f4ad165371771f5398faba397edf6778837a18599ea13ff
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    list(APPEND fetchContentPackagesToMakeAvailable brotli)
  endif()
endif()

if(AERONET_ENABLE_OPENTELEMETRY)
  # Prefer system-provided package if available
  find_package(opentelemetry-cpp QUIET)
  if(NOT TARGET opentelemetry::trace AND NOT TARGET OpenTelemetry::trace)
    include(FetchContent)
    # Require libcurl and protobuf development files. These are heavy native
    # dependencies and should be provided by the system package manager when
    # AERONET_ENABLE_OPENTELEMETRY=ON is used.
    find_package(CURL QUIET)
    find_package(Protobuf QUIET)
    if(NOT CURL_FOUND)
      message(FATAL_ERROR "AERONET_ENABLE_OPENTELEMETRY=ON requires libcurl development files to build the OTLP HTTP exporter. Install libcurl (e.g. libcurl4-openssl-dev) or set AERONET_ENABLE_OPENTELEMETRY=OFF.")
    endif()
    if(NOT Protobuf_FOUND)
      message(FATAL_ERROR "AERONET_ENABLE_OPENTELEMETRY=ON requires protobuf development files (libprotobuf & protoc). Install protobuf (e.g. libprotobuf-dev and protobuf-compiler) or set AERONET_ENABLE_OPENTELEMETRY=OFF.\n\nNote: fetching opentelemetry-cpp will also attempt to fetch/build protobuf which may fail in disconnected environments. Using system protobuf avoids that and reduces build-time fetches.")
    endif()

    # If a Conan-provided Protobuf package exposes a protoc target, prefer that
    # executable for any protobuf code generation. This prevents CMake/FetchContent
    # builds (like opentelemetry-cpp) from accidentally invoking a system
    # /usr/bin/protoc that is version-incompatible with the Protobuf headers
    # that will be used for compilation (see CI failures where generated files
    # were produced by a newer protoc than the headers available during compile).
    if(TARGET protobuf::protoc)
      # Try to read the imported location of the protoc executable target.
      get_target_property(_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION)
      if(NOT _PROTOC_EXECUTABLE)
        # Fallback property name used by some package configs.
        get_target_property(_PROTOC_EXECUTABLE protobuf::protoc LOCATION)
      endif()
      if(_PROTOC_EXECUTABLE)
        # Force the Protobuf CMake cache variable so subsequent find_package or
        # libraries that rely on Protobuf use this executable for generation.
        set(Protobuf_PROTOC_EXECUTABLE "${_PROTOC_EXECUTABLE}" CACHE FILEPATH "Protobuf protoc executable (from Conan)" FORCE)
      endif()
    endif()

    FetchContent_Declare(
      opentelemetry_cpp
      URL https://github.com/open-telemetry/opentelemetry-cpp/archive/refs/tags/v1.23.0.tar.gz
      URL_HASH SHA256=148ef298a4ef9e016228d53d7208ab9359d4fdf87f55649d60d07ffacc093b33
    )
    # Configure opentelemetry-cpp to enable OTLP HTTP exporter and a curl http client.
    # CRITICAL: Use nostd (WITH_STL=OFF) to ensure consistent ABI across all opentelemetry types.
    # Using WITH_STL=ON causes ABI mismatch where some types use nostd::variant while others
    # use std::shared_ptr, leading to variant destruction failures.
    set(WITH_STL OFF CACHE BOOL "Use nostd types for ABI consistency" FORCE)
    set(WITH_HTTP_CLIENT_CURL ON CACHE BOOL "Enable curl http client for OTLP HTTP" FORCE)
    set(WITH_OTLP_HTTP ON CACHE BOOL "Build OTLP HTTP exporter" FORCE)
    # Avoid pulling in gRPC/protobuf unless the user explicitly enables it upstream.
    set(WITH_OTLP_GRPC OFF CACHE BOOL "Disable OTLP gRPC exporter by default" FORCE)
    # Prefer to disable building tests/benchmarks/examples from the opentelemetry subtree
    # to avoid pulling additional build-time dependencies (like Google Benchmark) into consumers.
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(WITH_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WITH_FUNC_TESTS OFF CACHE BOOL "" FORCE)
    
    list(APPEND fetchContentPackagesToMakeAvailable opentelemetry_cpp)
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

  # Restore BUILD_SHARED_LIBS if we overrode it for zstd.
  if(DEFINED _AERONET_PREV_BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS ${_AERONET_PREV_BUILD_SHARED_LIBS} CACHE BOOL "Restore previous value" FORCE)
    unset(_AERONET_PREV_BUILD_SHARED_LIBS)
  endif()

  # Normalize brotli target names (create plain names if only *-static provided)
  if(AERONET_ENABLE_BROTLI)
    if(TARGET brotlicommon-static AND NOT TARGET brotlicommon)
      add_library(brotlicommon INTERFACE IMPORTED)
      target_link_libraries(brotlicommon INTERFACE brotlicommon-static)
    endif()
    if(TARGET brotlidec-static AND NOT TARGET brotlidec)
      add_library(brotlidec INTERFACE IMPORTED)
      target_link_libraries(brotlidec INTERFACE brotlidec-static)
    endif()
    if(TARGET brotlienc-static AND NOT TARGET brotlienc)
      add_library(brotlienc INTERFACE IMPORTED)
      target_link_libraries(brotlienc INTERFACE brotlienc-static)
    endif()
  endif()
endif()