# aeronet Installation & Build Guide

This document centralizes how to build, install, and consume **aeronet**.

> Cross-platform C++23 HTTP/1.1, HTTP/2 and WebSocket server and client library. Tested on Linux, macOS, and Windows.

## Toolchain & Platform

| Component | Minimum / Tested | Notes |
| --------- | ---------------- | ----- |
| OS | Linux (x86_64, aarch64), macOS (arm64, x86_64), Windows (x64) | Linux is primary (epoll); macOS uses kqueue; Windows uses WSAPoll |
| CMake | 3.28+ | Enforced at configure time; see [Glaze with CMake older than 3.31](#glaze-with-cmake-older-than-331) |
| C++ | C++23 | `CMAKE_CXX_STANDARD 23` required |
| Clang | 21.x | Earlier might work, not guaranteed |
| GCC | 13.x | GCC 12 may lack some C++23 pieces |
| MSVC | 17.x (VS 2022) | Windows builds; C++23 `/std:c++latest` |
| Apple Clang | Xcode 15+ | macOS builds |
| OpenSSL (opt) | 1.1.1 / 3.x | For TLS (HTTPS) support |
| spdlog (opt) | 1.11+ | Logging; header-only usage |
| glaze (opt) | 8.4+ | JSON / YAML support (`AERONET_ENABLE_GLAZE`); 9.0.0 when fetched |
| GoogleTest (tests) | 1.13+ | Auto-fetched if missing |

> **Note:** Some features are Linux-specific and auto-disabled on other platforms: kTLS, `MSG_ZEROCOPY`, `sendfile`, `eventfd`/`timerfd`, DogStatsD via Unix sockets. The core HTTP server works on all supported platforms.

### Glaze with CMake older than 3.31

Glaze is header-only, but its `CMakeLists.txt` requires CMake 3.31 since glaze 8.1. When aeronet fetches glaze with an older CMake, it uses the glaze headers without running the glaze `CMakeLists.txt`, and defines the `glaze::glaze` target itself, with the same compile options. The glaze CMake options (`glaze_DISABLE_SIMD_WHEN_SUPPORTED`, `glaze_DISABLE_ALWAYS_INLINE`...) then have no effect: define the matching `GLZ_*` macros (`GLZ_DISABLE_SIMD`, `GLZ_DISABLE_ALWAYS_INLINE`...) instead. A glaze found by `find_package` is used as is, whatever the CMake version.

## CMake Options

Every CMake option - build products, developer tools, protocol and integration features, with their defaults and dependencies - is listed in the [build configuration reference](../reference/configuration.md). Options default to `ON` when aeronet is the top-level project, and to `OFF` when it is consumed as a dependency (FetchContent, Conan, vcpkg), so enable the features you need explicitly in that case.

When aeronet is built once and not edited (as a dependency, in a package or a CI image), `AERONET_UNITY_BUILD=ON` builds its libraries several times faster: see the [unity build recipe](../reference/configuration.md#unity-build).

## Quick Builds

Release (static, TLS + zlib + zstd + brotli ON, tests OFF):

```bash
cmake -S . -B build -D CMAKE_BUILD_TYPE=Release \
  -D AERONET_ENABLE_OPENSSL=ON -D AERONET_ENABLE_ZLIB=ON -D AERONET_ENABLE_ZSTD=ON -D AERONET_ENABLE_BROTLI=ON \
  -D AERONET_BUILD_TESTS=OFF
cmake --build build --parallel
```

Debug with sanitizers + tests:

```bash
cmake -S . -B build-debug -D CMAKE_BUILD_TYPE=Debug \
  -D AERONET_ENABLE_ASAN=ON -D AERONET_BUILD_TESTS=ON
cmake --build build-debug --parallel
ctest --test-dir build-debug --output-on-failure
```

Debug with thread sanitizer + tests (a data race fails the test, TSan exiting with code 66):

```bash
cmake -S . -B build-tsan -D CMAKE_BUILD_TYPE=Debug \
  -D AERONET_ENABLE_TSAN=ON -D AERONET_BUILD_TESTS=ON
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure
```

Plain HTTP only (no TLS / extra codecs):

```bash
cmake -S . -B build-plain -G Ninja -D CMAKE_BUILD_TYPE=Release \
  -D AERONET_BUILD_TESTS=OFF \
  -D AERONET_ENABLE_OPENSSL=OFF -D AERONET_ENABLE_HTTP2=OFF \
  -D AERONET_ENABLE_WEBSOCKET=OFF -D AERONET_ENABLE_HTTP_CLIENT=OFF \
  -D AERONET_ENABLE_ZLIB=OFF -D AERONET_ENABLE_ZSTD=OFF \
  -D AERONET_ENABLE_BROTLI=OFF -D AERONET_ENABLE_SPDLOG=OFF \
  -D AERONET_ENABLE_GLAZE=OFF -D AERONET_ENABLE_OPENTELEMETRY=OFF
cmake --build build-plain --parallel
```

Enable glaze JSON support explicitly:

```bash
cmake -S . -B build-glaze -D CMAKE_BUILD_TYPE=Release -D AERONET_ENABLE_GLAZE=ON
cmake --build build-glaze --parallel
```

Shared libraries (minimal HTTP-only profile):

```bash
cmake -S . -B build-shared -G Ninja -D CMAKE_BUILD_TYPE=Release \
  -D AERONET_BUILD_SHARED=ON -D AERONET_BUILD_TESTS=OFF \
  -D AERONET_ENABLE_OPENSSL=OFF -D AERONET_ENABLE_HTTP2=OFF \
  -D AERONET_ENABLE_WEBSOCKET=OFF -D AERONET_ENABLE_HTTP_CLIENT=OFF \
  -D AERONET_ENABLE_ZLIB=OFF -D AERONET_ENABLE_ZSTD=OFF \
  -D AERONET_ENABLE_BROTLI=OFF -D AERONET_ENABLE_SPDLOG=OFF \
  -D AERONET_ENABLE_GLAZE=OFF -D AERONET_ENABLE_OPENTELEMETRY=OFF
cmake --build build-shared --parallel
```

## Continuous-integration coverage

The CI workflow is the tested compatibility matrix, rather than a promise that every possible toolchain is interchangeable. It exercises GCC 13 and Clang 21 on Ubuntu x86_64 with optional features both OFF and ON, plus a GCC Release smoke build; one of the Clang legs builds the libraries as unity builds (`AERONET_UNITY_BUILD`). Feature-enabled Debug and Release builds also run on Ubuntu ARM. macOS and Windows builds cover Debug and Release, and an Alpine/musl Debug job covers the minimal libc environment. The Examples and Docs job builds every example with all optional features enabled, runs the executable smoke tests, compiles the established Markdown C++ examples, and builds this MkDocs site with `--strict`.

Use the exact CI versions for a supported baseline. The [build configuration reference](../reference/configuration.md) maps each feature to its dependency and default.

### macOS

```bash
cmake -S . -B build -G Ninja -D CMAKE_BUILD_TYPE=Release \
  -D AERONET_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Windows (Visual Studio 2022)

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -D CMAKE_BUILD_TYPE=Release `
  -D AERONET_BUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

## C++ modules (experimental)

When `AERONET_BUILD_MODULES=ON` is set in CMake, the library builds an optional C++20 module interface (`aeronet`).
The module re-exports the public API surface of `<aeronet/aeronet.hpp>` - only user-facing types, configuration structs, handler callbacks, and protocol enums are exported; internal implementation details are omitted.

Requirements:

- CMake 3.28+
- A build system with C++20 module support (e.g. Ninja 1.11+)
- Compiler minimum versions: GCC 15, Clang 18, or MSVC 17.6

```bash
cmake -B build -G Ninja -D AERONET_BUILD_MODULES=ON
cmake --build build
```

```cpp
#include <utility> // std::move - standard library is not exported by the module

import aeronet;

using aeronet::HttpRequestView;
using aeronet::HttpResponse;
using aeronet::HttpServer;
using aeronet::HttpServerConfig;
using aeronet::Router;
using aeronet::http::Method;

int main() {
    Router router;
    router.setPath(Method::GET, "/hello", [](const HttpRequestView& req) {
        return HttpResponse(200).header("x-req-body", req.body()).body("hello from aeronet\n");
    });
    HttpServer server(HttpServerConfig{}, std::move(router));
    server.run();
}
```

## Install

```bash
cmake -S . -B build -D CMAKE_BUILD_TYPE=Release -D AERONET_INSTALL=ON
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
  # Only build the aeronet libraries your targets link (aeronet_server alone does not build the client)
  EXCLUDE_FROM_ALL
)

# Enable the features you want before FetchContent_MakeAvailable
set(AERONET_ENABLE_OPENSSL ON CACHE BOOL "" FORCE)
set(AERONET_ENABLE_ZSTD ON CACHE BOOL "" FORCE)
set(AERONET_ENABLE_BROTLI OFF CACHE BOOL "" FORCE) # toggle as needed
set(AERONET_ENABLE_SPDLOG OFF CACHE BOOL "" FORCE)
set(AERONET_ENABLE_WEBSOCKET OFF CACHE BOOL "" FORCE) # toggle as needed
set(AERONET_UNITY_BUILD ON CACHE BOOL "" FORCE) # several times faster to build, see the unity build recipe

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
cmake -S . -B build -D CMAKE_TOOLCHAIN_FILE=build/conan/conan_toolchain.cmake \
  -D CMAKE_BUILD_TYPE=Release
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

The port maps `VCPKG_LIBRARY_LINKAGE=dynamic` to `-D AERONET_BUILD_SHARED=ON` automatically.
