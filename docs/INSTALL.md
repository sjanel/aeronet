# aeronet Installation & Build Guide

This document centralizes how to build, install, and consume **aeronet**.

> Cross-platform C++23 HTTP/1.1 server library (optional TLS). Tested on Linux, macOS, and Windows.

## Toolchain & Platform

| Component | Minimum / Tested | Notes |
| --------- | ---------------- | ----- |
| OS | Linux (x86_64, aarch64), macOS (arm64, x86_64), Windows (x64) | Linux is primary (epoll); macOS uses kqueue; Windows uses WSAPoll |
| CMake | 3.28+ | Enforced at configure time |
| C++ | C++23 | `CMAKE_CXX_STANDARD 23` required |
| Clang | 21.x | Earlier might work, not guaranteed |
| GCC | 13.x | GCC 12 may lack some C++23 pieces |
| MSVC | 17.x (VS 2022) | Windows builds; C++23 `/std:c++latest` |
| Apple Clang | Xcode 15+ | macOS builds |
| OpenSSL (opt) | 1.1.1 / 3.x | For TLS (HTTPS) support |
| spdlog (opt) | 1.11+ | Logging; header-only usage |
| glaze (opt) | 7.0.2+ | JSON serialization support (`AERONET_ENABLE_GLAZE`) |
| GoogleTest (tests) | 1.13+ | Auto-fetched if missing |

> **Note:** Some features are Linux-specific and auto-disabled on other platforms: kTLS, `MSG_ZEROCOPY`, `sendfile`, `eventfd`/`timerfd`, DogStatsD via Unix sockets. The core HTTP server works on all supported platforms.

## CMake Options

| Option | Default* | Purpose |
| ------ | -------- | ------- |
| `AERONET_BUILD_EXAMPLES` | ON* | Build example programs |
| `AERONET_BUILD_TESTS` | ON* | Build unit tests (needs GTest) |
| `AERONET_BUILD_SHARED` | OFF | Build shared instead of static libs |
| `AERONET_BUILD_MODULES` | OFF | Build as C++ modules (experimental) |
| `AERONET_BUILD_BENCHMARKS` | ON top-level except Debug | Build benchmark executables and selected comparison backends |
| `AERONET_INSTALL` | ON* | Enable install + package config export |
| `AERONET_ENABLE_CCACHE` | ON* | Use ccache when it is installed |
| `AERONET_ENABLE_SPDLOG` | ON* | Enable spdlog logging integration |
| `AERONET_ENABLE_OPENSSL` | ON* | Enable TLS module (`aeronet_tls`) |
| `AERONET_ENABLE_GLAZE` | ON* | Enable glaze-based JSON serialization helpers |
| `AERONET_ENABLE_OPENTELEMETRY` | ON* | Enable OpenTelemetry instrumentation (build-time flag; opt-in) |
| `AERONET_ENABLE_WEBSOCKET` | ON | Enable WebSocket protocol support |
| `AERONET_ENABLE_ASYNC_HANDLERS` | ON | Enable asynchronous routing handlers |
| `AERONET_ENABLE_HTTP2` | ON | Enable HTTP/2 protocol support |
| `AERONET_ENABLE_HTTP_CLIENT` | ON | Enable the synchronous `HttpClient` module |
| `AERONET_ENABLE_JWT` | ON when OpenSSL + Glaze are ON | Enable JWS-profile JWT/JWKS support; forced OFF without both prerequisites |
| `AERONET_ENABLE_ZLIB` | ON* | Enable gzip/deflate (zlib / zlib-ng) compression + decompression |
| `AERONET_ENABLE_ZLIBNG` | ON | Use `zlib-ng` implementation instead of classic `zlib` |
| `AERONET_ENABLE_ZSTD` | ON* | Enable zstd compression + decompression |
| `AERONET_ENABLE_BROTLI` | ON* | Enable brotli compression + decompression |
| `AERONET_ENABLE_ASAN` | OFF | Address/UB sanitizers in debug builds |
| `AERONET_ENABLE_CLANG_TIDY` | OFF | Run clang-tidy on targets |
| `AERONET_WARNINGS_AS_ERRORS` | OFF | Treat warnings as errors |
| `AERONET_ASAN_OPTIONS` | (preset) | Override sanitizer flags |
| `AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS` | OFF | Extra custom runtime memory checks |
| `AERONET_ENABLE_TEST_HOOKS` | non-Release tests | Enable test-only transport hooks; keep OFF in production |
| `AERONET_ENABLE_WARNINGS` | ON* | Enable the project warning set |

*Defaults apply when aeronet is the top-level project; they flip to OFF when used as a dependency.

## Quick Builds

Release (static, TLS + zlib + zstd + brotli ON, tests OFF):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DAERONET_ENABLE_OPENSSL=ON -DAERONET_ENABLE_ZLIB=ON -DAERONET_ENABLE_ZSTD=ON -DAERONET_ENABLE_BROTLI=ON \
  -DAERONET_BUILD_TESTS=OFF
cmake --build build --parallel
```

Debug with sanitizers + tests:

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DAERONET_ENABLE_ASAN=ON -DAERONET_BUILD_TESTS=ON
cmake --build build-debug --parallel
ctest --test-dir build-debug --output-on-failure
```

Plain HTTP only (no TLS / extra codecs):

```bash
cmake -S . -B build-plain -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DAERONET_BUILD_TESTS=OFF \
  -DAERONET_ENABLE_OPENSSL=OFF -DAERONET_ENABLE_HTTP2=OFF \
  -DAERONET_ENABLE_WEBSOCKET=OFF -DAERONET_ENABLE_HTTP_CLIENT=OFF \
  -DAERONET_ENABLE_ZLIB=OFF -DAERONET_ENABLE_ZSTD=OFF \
  -DAERONET_ENABLE_BROTLI=OFF -DAERONET_ENABLE_SPDLOG=OFF \
  -DAERONET_ENABLE_GLAZE=OFF -DAERONET_ENABLE_OPENTELEMETRY=OFF
cmake --build build-plain --parallel
```

Enable glaze JSON support explicitly:

```bash
cmake -S . -B build-glaze -DCMAKE_BUILD_TYPE=Release -DAERONET_ENABLE_GLAZE=ON
cmake --build build-glaze --parallel
```

Shared libraries (minimal HTTP-only profile):

```bash
cmake -S . -B build-shared -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DAERONET_BUILD_SHARED=ON -DAERONET_BUILD_TESTS=OFF \
  -DAERONET_ENABLE_OPENSSL=OFF -DAERONET_ENABLE_HTTP2=OFF \
  -DAERONET_ENABLE_WEBSOCKET=OFF -DAERONET_ENABLE_HTTP_CLIENT=OFF \
  -DAERONET_ENABLE_ZLIB=OFF -DAERONET_ENABLE_ZSTD=OFF \
  -DAERONET_ENABLE_BROTLI=OFF -DAERONET_ENABLE_SPDLOG=OFF \
  -DAERONET_ENABLE_GLAZE=OFF -DAERONET_ENABLE_OPENTELEMETRY=OFF
cmake --build build-shared --parallel
```

## Continuous-integration coverage

The CI workflow is the tested compatibility matrix, rather than a promise that every possible toolchain is interchangeable. It exercises GCC 13 and Clang 21 on Ubuntu x86_64 with optional features both OFF and ON, plus a GCC Release smoke build. Feature-enabled Debug and Release builds also run on Ubuntu ARM. macOS and Windows builds cover Debug and Release, and an Alpine/musl Debug job covers the minimal libc environment. The Examples and Docs job builds every example with all optional features enabled, runs the executable smoke tests, compiles the established Markdown C++ examples, and builds this MkDocs site with `--strict`.

Use the exact CI versions for a supported baseline. The [build configuration reference](reference/configuration.md) maps each feature to its dependency and default.

### macOS

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DAERONET_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Windows (Visual Studio 2022)

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_BUILD_TYPE=Release `
  -DAERONET_BUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

## Install

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAERONET_INSTALL=ON
cmake --build build --parallel
cmake --install build --prefix "$(pwd)/dist"
```

Layout (abbrev):

```text
dist/
  include/aeronet/...    (headers)
  lib/libaeronet*.a/.so  (core libs)
  lib/cmake/aeronet/     (aeronetConfig.cmake)
```

Consume installed package:

```cmake
find_package(aeronet CONFIG REQUIRED)
add_executable(app src/app.cpp)
target_link_libraries(app PRIVATE aeronet)
if (TARGET aeronet_tls)
  target_link_libraries(app PRIVATE aeronet_tls)
endif()
```

## FetchContent Integration

```cmake
include(FetchContent)

FetchContent_Declare(
  aeronet
  GIT_REPOSITORY https://github.com/sjanel/aeronet.git
  GIT_TAG main # Or specify a release tag, e.g., v1.0.0
)

# Enable the features you want before FetchContent_MakeAvailable
set(AERONET_ENABLE_OPENSSL ON CACHE BOOL "" FORCE)
set(AERONET_ENABLE_ZSTD ON CACHE BOOL "" FORCE)
set(AERONET_ENABLE_BROTLI OFF CACHE BOOL "" FORCE) # toggle as needed
set(AERONET_ENABLE_SPDLOG OFF CACHE BOOL "" FORCE)
set(AERONET_ENABLE_WEBSOCKET OFF CACHE BOOL "" FORCE) # toggle as needed

FetchContent_MakeAvailable(aeronet)

add_executable(my_server src/my_server.cpp)
target_link_libraries(my_server PRIVATE aeronet_server)
```

## Package Managers

### Conan (v2)

A minimal `conanfile.py` is provided at repository root. Example consumer `conanfile.txt`:

```text
[requires]
aeronet/$(cat VERSION)

[generators]
CMakeToolchain
CMakeDeps
```

Install & build (Release static):

```bash
conan install . --output-folder=build/conan -s build_type=Release \
  -o aeronet:with_openssl=True -o aeronet:with_spdlog=False
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=build/conan/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Linking in CMake after `find_package(aeronet CONFIG)` works the same (Conan generated files expose targets).

Available Conan options map:

| Conan Option | Effect | Maps to CMake |
| ------------ | ------ | ------------- |
  | `shared` | Build shared libs | `AERONET_BUILD_SHARED` |
  | `with_openssl` | TLS support | `AERONET_ENABLE_OPENSSL` |
  | `with_spdlog` | Logging integration | `AERONET_ENABLE_SPDLOG` |
  | `with_zlib` | gzip/deflate support | `AERONET_ENABLE_ZLIB` |
  | `with_zstd` | zstd support | `AERONET_ENABLE_ZSTD` |
  | `with_br` | brotli support (conan option name in recipe) | `AERONET_ENABLE_BROTLI` |
  | `with_opentelemetry` | Enable OpenTelemetry instrumentation (pulls opentelemetry-cpp & protobuf) | `AERONET_ENABLE_OPENTELEMETRY` |
  | `with_glaze` | Enable glaze JSON support | `AERONET_ENABLE_GLAZE` |

### vcpkg (Overlay Port)

An experimental port lives in `ports/aeronet`. You can use it as an overlay until (if) upstreamed:

Note: the port is experimental. Two common ways to consume the overlay port:

#### Classic command-line (overlay-ports env)

```bash
VCPKG_OVERLAY_PORTS=./ports vcpkg install aeronet --triplet x64-linux
```

### Manifest mode (recommended for reproducible builds)

Create or update `vcpkg.json` in your project and add the overlay when invoking vcpkg:

```json
{
  "name": "my-project",
  "version": "0.1.0",
  "dependencies": [ "aeronet" ]
}
```

Then install with:

```bash
VCPKG_OVERLAY_PORTS=./ports vcpkg install --triplet x64-linux
```

Enable TLS and specific compression features explicitly (all codecs opt-in except zlib default when top-level):

```bash
VCPKG_OVERLAY_PORTS=./ports vcpkg install aeronet[tls,zstd,brotli,spdlog] --triplet x64-linux
```

Feature switches (examples):

```bash
# Minimal (zlib only)
VCPKG_OVERLAY_PORTS=./ports vcpkg install aeronet --triplet x64-linux
# Add TLS
VCPKG_OVERLAY_PORTS=./ports vcpkg install aeronet[tls] --triplet x64-linux
# Full (TLS + zstd + brotli + spdlog)
VCPKG_OVERLAY_PORTS=./ports vcpkg install aeronet[tls,zstd,brotli,spdlog] --triplet x64-linux
```

In your CMake project (after integrating vcpkg toolchain):

```cmake
find_package(aeronet CONFIG REQUIRED)

add_executable(app main.cpp)

target_link_libraries(app PRIVATE aeronet)
```

Shared libraries via vcpkg: use (or create) a dynamic triplet, e.g.:

```bash
cp vcpkg/triplets/community/x64-linux.cmake x64-linux-dynamic.cmake
echo "set(VCPKG_LIBRARY_LINKAGE dynamic)" >> x64-linux-dynamic.cmake
vcpkg install aeronet --overlay-ports=./ports --triplet x64-linux-dynamic
```

The port maps `VCPKG_LIBRARY_LINKAGE=dynamic` to `-DAERONET_BUILD_SHARED=ON` automatically.
