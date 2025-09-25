# Aeronet Installation & Build Guide

This document centralizes how to build, install, and consume **Aeronet**.

> Linux‑only C++23 HTTP/1.1 server library (optional TLS). Tested with Clang 21 / GCC 13.

## Toolchain & Platform

| Component | Minimum / Tested | Notes |
|-----------|------------------|-------|
| OS | Linux (x86_64) | epoll required; no Windows/macOS support |
| CMake | 3.28+ | Enforced at configure time |
| C++ | C++23 | `CMAKE_CXX_STANDARD 23` required |
| Clang | 21.x | Earlier might work, not guaranteed |
| GCC | 13.x | GCC 12 may lack some C++23 pieces |
| OpenSSL (opt) | 1.1.1 / 3.x | For TLS (HTTPS) support |
| spdlog (opt) | 1.11+ | Logging; header-only usage |
| GoogleTest (tests) | 1.13+ | Auto-fetched if missing |

## CMake Options

| Option | Default* | Purpose |
|--------|----------|---------|
| `AERONET_BUILD_EXAMPLES` | ON* | Build example programs |
| `AERONET_BUILD_TESTS` | ON* | Build unit tests (needs GTest) |
| `AERONET_BUILD_SHARED` | OFF | Build shared instead of static libs |
| `AERONET_INSTALL` | ON* | Enable install + package config export |
| `AERONET_ENABLE_SPDLOG` | OFF | Enable spdlog logging integration |
| `AERONET_ENABLE_OPENSSL` | OFF | Enable TLS module (`aeronet_tls`) |
| `AERONET_ENABLE_ASAN` | ON (Debug) | Address/UB sanitizers in debug builds |
| `AERONET_ENABLE_CLANG_TIDY` | OFF | Run clang-tidy on targets |
| `AERONET_WARNINGS_AS_ERRORS` | OFF | Treat warnings as errors |
| `AERONET_ASAN_OPTIONS` | (preset) | Override sanitizer flags |

*Defaults apply when Aeronet is the top-level project; they flip to OFF when added via `add_subdirectory`, except `AERONET_ENABLE_OPENSSL` which remains ON by default if OpenSSL is available.

## Quick Builds

Release (static, TLS ON, tests OFF):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DAERONET_ENABLE_OPENSSL=ON -DAERONET_BUILD_TESTS=OFF
cmake --build build -j
```

Debug with sanitizers + tests:

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DAERONET_ENABLE_ASAN=ON -DAERONET_BUILD_TESTS=ON
cmake --build build-debug -j
ctest --test-dir build-debug --output-on-failure
```

Plain HTTP only (default):

```bash
cmake -S . -B build-plain -DCMAKE_BUILD_TYPE=Release
cmake --build build-plain -j
```

Shared libraries:

```bash
cmake -S . -B build-shared -DCMAKE_BUILD_TYPE=Release -DAERONET_BUILD_SHARED=ON
cmake --build build-shared -j
```

## Install

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAERONET_INSTALL=ON
cmake --build build -j
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
  GIT_TAG main
)

# Enable the features you want (spdlog, openssl, etc)
set(AERONET_ENABLE_XXXX ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(aeronet)

add_executable(my_server src/my_server.cpp)
target_link_libraries(my_server PRIVATE aeronet)
if (AERONET_ENABLE_OPENSSL AND TARGET aeronet_tls)
  target_link_libraries(my_server PRIVATE aeronet_tls)
endif()
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
cmake --build build -j
```

Linking in CMake after `find_package(aeronet CONFIG)` works the same (Conan generated files expose targets).

Available Conan options map:

| Conan Option | Effect | Maps to CMake |
|--------------|--------|---------------|
| `shared` | Build shared libs | `AERONET_BUILD_SHARED` |
| `with_openssl` | TLS support | `AERONET_ENABLE_OPENSSL` |
| `with_spdlog` | Logging integration | `AERONET_ENABLE_SPDLOG` |

### vcpkg (Overlay Port)

An experimental port lives in `ports/aeronet`. You can use it as an overlay until (if) upstreamed:

```bash
vcpkg install aeronet --overlay-ports=./ports --triplet x64-linux
```

Enable TLS explicitly (TLS is now opt-in):

```bash
vcpkg install aeronet[tls] --overlay-ports=./ports --triplet x64-linux
```

Enable spdlog feature:

```bash
vcpkg install aeronet[spdlog] --overlay-ports=./ports --triplet x64-linux
```

In your CMake project (after integrating vcpkg toolchain):

```cmake
find_package(aeronet CONFIG REQUIRED)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE aeronet)
if (TARGET aeronet_tls)
  target_link_libraries(app PRIVATE aeronet_tls)
endif()
```

Shared libraries via vcpkg: use (or create) a dynamic triplet, e.g.:

```bash
cp vcpkg/triplets/community/x64-linux.cmake x64-linux-dynamic.cmake
echo "set(VCPKG_LIBRARY_LINKAGE dynamic)" >> x64-linux-dynamic.cmake
vcpkg install aeronet --overlay-ports=./ports --triplet x64-linux-dynamic
```

The port maps `VCPKG_LIBRARY_LINKAGE=dynamic` to `-DAERONET_BUILD_SHARED=ON` automatically.

## CI Matrix (Suggested)

| Compiler | TLS | Build | Notes |
|----------|-----|-------|-------|
| Clang 21 | ON  | Debug | Sanitizers + tests |
| GCC 13   | ON  | Release | Production profile |
| Clang 21 | OFF | Release | TLS-off regression guard |

## Troubleshooting

| Symptom | Cause / Resolution |
|---------|--------------------|
| Missing `aeronet_tls` | Built with `AERONET_ENABLE_OPENSSL=OFF` or OpenSSL not found |
| Link errors (spdlog) | Enable `AERONET_ENABLE_SPDLOG` in both producer & consumer or disable it everywhere |
| Sanitizer libs missing | Install compiler runtime packages (e.g. `libasan`, `libubsan`) |
| Tests not built | Built via root project only (Conan package omits tests/examples) |

## Minimal Example

```cpp
#include <aeronet/http-server.hpp>
#include <aeronet/http-server-config.hpp>
#include <aeronet/http-response.hpp>
using namespace aeronet;
int main(){
  HttpServer server(HttpServerConfig{}.withPort(8080));
  server.setHandler([](const HttpRequest&){
    HttpResponse r{200, "OK"};
    r.contentType = "text/plain"; r.body = "hello\n"; return r; });
  server.run();
}
```

## License

MIT – see [LICENSE](../LICENSE).

---

Questions or wanting Conan/vcpkg packaging examples? Open an issue.
