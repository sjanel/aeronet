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
      URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.3.tar.gz
      URL_HASH SHA256=15a04e69c222eb6c01094b5c7ff8a249b36bb22788d72519646fb85feb267e67
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    list(APPEND fetchContentPackagesToMakeAvailable spdlog)
  endif()
  set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
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

    set(zstd_URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz)
    set(zstd_URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3)

    FetchContent_Declare(
      zstd
      URL "${zstd_URL}"
      URL_HASH "${zstd_URL_HASH}"
      SOURCE_SUBDIR build/cmake
    )
    list(APPEND fetchContentPackagesToMakeAvailable zstd)
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