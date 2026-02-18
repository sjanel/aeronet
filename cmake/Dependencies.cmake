include(FetchContent)
include(cmake/AeronetFindOrFetch.cmake)

set(AeronetFetchContentPackagesToMakeAvailable "")

# Some imported targets provided by FetchContent or package configs only set
# IMPORTED_LOCATION_RELEASE (or RELEASE-only properties). When CMake later
# asks for IMPORTED_LOCATION (or the Debug configuration) this can trigger
# "IMPORTED_LOCATION not set" errors. Normalize common imported targets by
# copying RELEASE -> DEBUG and setting IMPORTED_LOCATION if missing.
function(_aeronet_normalize_imported_location target)
  if(NOT TARGET ${target})
    return()
  endif()
  # Read config-specific properties
  get_target_property(_loc_debug ${target} IMPORTED_LOCATION_DEBUG)
  get_target_property(_loc_release ${target} IMPORTED_LOCATION_RELEASE)
  get_target_property(_loc_no_cfg ${target} IMPORTED_LOCATION)

  if(NOT _loc_debug AND _loc_release)
    set_target_properties(${target} PROPERTIES IMPORTED_LOCATION_DEBUG "${_loc_release}")
  endif()

  # Ensure IMPORTED_LOCATION exists (some consumers query this property directly)
  get_target_property(_loc_now ${target} IMPORTED_LOCATION)
  if(NOT _loc_now)
    if(_loc_debug)
      set_target_properties(${target} PROPERTIES IMPORTED_LOCATION "${_loc_debug}")
    elseif(_loc_release)
      set_target_properties(${target} PROPERTIES IMPORTED_LOCATION "${_loc_release}")
    endif()
  endif()
endfunction()

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
  list(APPEND AeronetFetchContentPackagesToMakeAvailable amadeusamc)
endif()

if(AERONET_BUILD_TESTS)
  # Use modern position independent etc.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

  aeronet_find_or_declare(
    NAME googletest
    CONFIG
    TARGETS GTest::gtest GTest::gmock GTest::gtest_main
    DECLARE
      URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
      URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )

  enable_testing()
endif()

if(AERONET_ENABLE_SPDLOG)
  aeronet_find_or_declare(
    NAME spdlog
    CONFIG
    TARGETS spdlog::spdlog
    DECLARE
      URL https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz
      URL_HASH SHA256=d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
endif()

# Compression libraries (optional, isolated like TLS). Detect BEFORE building core targets so
# compile definitions propagate correctly to aeronet library sources.
if(AERONET_ENABLE_ZLIB)
  # Ensure zlib is built with -fPIC so it can be linked into shared libraries (e.g., drogon in benchmarks)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)

  aeronet_find_or_declare(
    NAME ZLIB
    CONFIG
    TARGETS ZLIB::ZLIBSTATIC
    DECLARE
      URL https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz
      URL_HASH SHA256=b99a0b86c0ba9360ec7e78c4f1e43b1cbdf1e6936c8fa0f6835c0cd694a495a1
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
endif()

if(AERONET_ENABLE_ZSTD)
  # Configure zstd BEFORE declaration
  set(ZSTD_BUILD_STATIC ON  CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)

  aeronet_find_or_declare(
    NAME zstd
    CONFIG
    TARGETS zstd::libzstd
    DECLARE
      URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
      URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
      SOURCE_SUBDIR build/cmake
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
endif()

if(AERONET_ENABLE_BROTLI)
  # Brotli-specific configuration (applies only when fetched)
  set(BROTLI_DISABLE_TESTS ON CACHE BOOL "Disable brotli tests" FORCE)

  # Brotli does not reliably provide Debug libs on many systems,
  # so policy decides whether system packages are allowed.
  aeronet_find_or_declare(
    NAME brotli
    CONFIG
    TARGETS
      brotlicommon
      brotlidec
      brotlienc
    DECLARE
      URL https://github.com/google/brotli/archive/refs/tags/v1.2.0.tar.gz
      URL_HASH SHA256=816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
endif()

if(AERONET_ENABLE_OPENTELEMETRY)

  aeronet_system_packages_allowed(_otel_allow_system)

  set(_USE_SYSTEM_OPENTELEMETRY FALSE)

  if(_otel_allow_system)
    find_package(opentelemetry-cpp CONFIG QUIET)
  endif()
  if(opentelemetry-cpp_FOUND)

    # Check required OTLP HTTP exporter
    set(_otel_has_http FALSE)
    if(TARGET opentelemetry-cpp::otlp_http_exporter
       OR TARGET opentelemetry-cpp::otlp_http_client
       OR TARGET opentelemetry-cpp::otlp_http_metric_exporter)
      set(_otel_has_http TRUE)
    endif()

    # Check proto target or headers
    set(_otel_has_proto FALSE)
    if(TARGET opentelemetry-cpp::proto OR TARGET opentelemetry_proto)
      set(_otel_has_proto TRUE)
    elseif(EXISTS "/usr/local/include/opentelemetry/proto/collector/metrics/v1/metrics_service.pb.h")
      set(_otel_has_proto TRUE)
    endif()

    if(_otel_has_http AND _otel_has_proto)
      message(STATUS "Using system opentelemetry-cpp with OTLP HTTP + proto support")
      set(_USE_SYSTEM_OPENTELEMETRY TRUE)
    else()
      message(STATUS "System opentelemetry-cpp missing required OTLP HTTP/proto components")
    endif()

  endif()
  if(NOT _USE_SYSTEM_OPENTELEMETRY)

    # Hard requirements (never FetchContent these)
    find_package(CURL CONFIG QUIET)
    if(NOT CURL_FOUND)
      find_package(CURL QUIET)
    endif()

    find_package(Protobuf CONFIG QUIET)
    if(NOT Protobuf_FOUND)
      find_package(Protobuf QUIET)
    endif()

    if(NOT CURL_FOUND)
      message(FATAL_ERROR
        "AERONET_ENABLE_OPENTELEMETRY requires libcurl development files.")
    endif()

    if(NOT Protobuf_FOUND)
      message(FATAL_ERROR
        "AERONET_ENABLE_OPENTELEMETRY requires protobuf (libprotobuf + protoc).")
    endif()
    if(TARGET protobuf::protoc)
      get_target_property(_PROTOC protobuf::protoc IMPORTED_LOCATION)
      if(NOT _PROTOC)
        get_target_property(_PROTOC protobuf::protoc LOCATION)
      endif()
      if(_PROTOC)
        set(Protobuf_PROTOC_EXECUTABLE
            "${_PROTOC}"
            CACHE FILEPATH "Protobuf protoc executable" FORCE)
      endif()
    endif()
    # ABI + feature configuration
    set(WITH_STL OFF CACHE BOOL "Use nostd for ABI stability" FORCE)
    set(WITH_HTTP_CLIENT_CURL ON CACHE BOOL "" FORCE)
    set(WITH_OTLP_HTTP ON CACHE BOOL "" FORCE)
    set(WITH_OTLP_GRPC OFF CACHE BOOL "" FORCE)

    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(WITH_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WITH_FUNC_TESTS OFF CACHE BOOL "" FORCE)

    set(WITH_ABI_VERSION_1 OFF CACHE BOOL "Use ABI v1" FORCE)
    set(WITH_ABI_VERSION_2 ON CACHE BOOL "Use ABI v2" FORCE)

    aeronet_find_or_declare(
      NAME opentelemetry_cpp
      TARGETS
        opentelemetry-cpp::otlp_http_exporter
      DECLARE
        URL https://github.com/open-telemetry/opentelemetry-cpp/archive/refs/tags/v1.25.0.tar.gz
        URL_HASH SHA256=a0c944a9de981fe1874b31d1fe44b830fc30ee030efa27ee23fc73012a3a13e9
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

  endif()

endif()

if(AERONET_ENABLE_GLAZE)
  aeronet_find_or_declare(
    NAME glaze
    CONFIG
    TARGETS glaze::glaze
    DECLARE
      URL https://github.com/stephenberry/glaze/archive/refs/tags/v7.0.2.tar.gz
      URL_HASH SHA256=febbec555648b310c2a1975ca750939cd00c4801dede8362fcf84cab7b3ae46f
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
endif()

# Make fetch content available
if(AeronetFetchContentPackagesToMakeAvailable)
  message(STATUS "Configuring packages ${AeronetFetchContentPackagesToMakeAvailable}")
  # In vcpkg builds, FETCHCONTENT_FULLY_DISCONNECTED is forced ON, so
  # population of new content that is not already present will fail.
  # We let FetchContent attempt population; if it fails to create the
  # expected targets we degrade gracefully by disabling AMC linkage.
  FetchContent_MakeAvailable(${AeronetFetchContentPackagesToMakeAvailable})

  # Create ZLIB::ZLIB alias for backward compatibility with zlib 1.3.2+
  # When building static-only, zlib creates ZLIB::ZLIBSTATIC but not ZLIB::ZLIB
  if(AERONET_ENABLE_ZLIB AND TARGET zlibstatic AND NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
  endif()

  # Restore BUILD_SHARED_LIBS if we overrode it for zstd.
  if(DEFINED _AERONET_PREV_BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS ${_AERONET_PREV_BUILD_SHARED_LIBS} CACHE BOOL "Restore previous value" FORCE)
    unset(_AERONET_PREV_BUILD_SHARED_LIBS)
  endif()
endif()