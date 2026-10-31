# Build configuration

aeronet is deliberately modular. CMake options select code and dependencies at configure time; runtime configuration only enables behavior that was compiled in. Options that use `PROJECT_IS_TOP_LEVEL` default to `ON` from an aeronet checkout and `OFF` when aeronet is brought in with `FetchContent` or another package manager.

## Build products and developer tools

| Option | Top-level default | Meaning |
| --- | --- | --- |
| `AERONET_INSTALL` | ON | Generate install rules and the CMake package export. |
| `AERONET_BUILD_EXAMPLES` | ON | Build the programs in `examples/`; OFF for a dependency. |
| `AERONET_BUILD_TESTS` | ON | Build unit and integration tests; OFF for a dependency. |
| `AERONET_BUILD_BENCHMARKS` | ON except Debug | Build benchmarks and their selected comparison backends. |
| `AERONET_BUILD_SHARED` | OFF | Build shared libraries instead of static libraries. Recommended for development: editing a library source then relinks only that library, not the test executables. The dependencies that aeronet downloads are static libraries either way, linked into the aeronet libraries (`BUILD_SHARED_LIBS` does not apply to them). |
| `AERONET_COMPACT_DEBUG_INFO` | ON for Debug with tests | Split DWARF (`-gsplit-dwarf`) + zstd-compressed debug sections (zlib fallback), on Linux with GCC or Clang. Divides the size of the Debug test executables by ~3.5. The `.dwo` files stay in the build tree, so keep it OFF for builds that are installed elsewhere. |
| `AERONET_BUILD_MODULES` | OFF | Experimental C++ module build. |
| `AERONET_UNITY_BUILD` | OFF | Build the aeronet libraries (not the tests) as CMake unity builds, in batches of 16 sources (`CMAKE_UNITY_BUILD_BATCH_SIZE` overrides it). Divides their compilation CPU time by 4 to 6 in Release, at the cost of slower incremental rebuilds: see the [unity build recipe](#unity-build). |
| `AERONET_ENABLE_CCACHE` | top-level | Use `ccache` when it is available. |
| `AERONET_ENABLE_TEST_HOOKS` | non-Release tests | Enable transport hooks used by tests. Do not enable in production builds. |
| `AERONET_ENABLE_ASAN` | OFF | Enable AddressSanitizer, UndefinedBehaviorSanitizer, and float-divide checks. |
| `AERONET_ENABLE_TSAN` | OFF | Enable ThreadSanitizer. Cannot be combined with `AERONET_ENABLE_ASAN`. |
| `AERONET_ASAN_OPTIONS` / `AERONET_TSAN_OPTIONS` | preset | Override the compile and link flags of the AddressSanitizer / ThreadSanitizer builds. |
| `AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS` | OFF | Enable aeronet's additional runtime memory checks. |
| `AERONET_ENABLE_CLANG_TIDY` | OFF | Run clang-tidy during the build, on the translation units and on every header (see the [static analysis recipe](#static-analysis)). `-D CLANG_TIDY=<path>` selects the executable (the first `clang-tidy` of `PATH` by default). With `AERONET_WARNINGS_AS_ERRORS`, a finding fails the build. |
| `AERONET_ENABLE_WARNINGS` | top-level | Enable the project warning set. |
| `AERONET_WARNINGS_AS_ERRORS` | OFF | Promote warnings to errors in Debug builds, except for the benchmark executables built on competitor frameworks, whose warnings are kept as warnings. |
| `AERONET_SPDLOG_USE_STD_FORMAT` | ON | Use `std::format` rather than fmt through spdlog. |

## Protocol and integration features

| Option | Default | Provides | Dependency or relationship |
| --- | --- | --- | --- |
| `AERONET_ENABLE_OPENSSL` | top-level | HTTPS, TLS client/server support, ALPN, mTLS, session tickets | OpenSSL |
| `AERONET_ENABLE_HTTP2` | ON | HTTP/2, HPACK, h2c, and ALPN negotiation | Pair with OpenSSL for HTTP/2 over TLS |
| `AERONET_ENABLE_WEBSOCKET` | ON | RFC 6455 endpoints | `AERONET_ENABLE_ZLIB` additionally enables permessage-deflate |
| `AERONET_ENABLE_HTTP_CLIENT` | ON | Synchronous `HttpClient`, pool, proxy, cache, retry | OpenSSL for HTTPS; HTTP/2 option for h2 client |
| `AERONET_ENABLE_HTTP_SERVER` | ON | Activates `HttpServer` & `SingleHttpServer` servers and handlers | OpenSSL for HTTPS; HTTP/2 option for h2 client |
| `AERONET_ENABLE_ASYNC_HANDLERS` | ON | Coroutine routing APIs | No external dependency |
| `AERONET_ENABLE_ZLIB` | top-level | gzip and deflate encoders/decoders | zlib-ng is preferred by default |
| `AERONET_ENABLE_ZLIBNG` | ON | Use zlib-ng in place of classic zlib | Meaningful only with zlib enabled |
| `AERONET_ENABLE_ZSTD` | top-level | zstd encoders/decoders | zstd |
| `AERONET_ENABLE_BROTLI` | top-level | Brotli encoders/decoders | Brotli |
| `AERONET_ENABLE_SPDLOG` | top-level | spdlog integration | spdlog |
| `AERONET_ENABLE_OPENTELEMETRY` | top-level | OpenTelemetry traces/metrics exporter | OpenTelemetry C++, curl, protobuf/protoc. An installed opentelemetry-cpp with the OTLP HTTP exporter is used (Release builds, vcpkg, Conan), otherwise it is downloaded. With vcpkg and Conan, an opentelemetry-cpp package without the OTLP HTTP exporter is a configure error instead |
| `AERONET_ENABLE_GLAZE` | top-level | JSON/YAML config loading and serialization helpers | glaze |
| `AERONET_ENABLE_PROTOBUF` | top-level | Protocol Buffers support. Header-only: the aeronet libraries do not use protobuf, the code including the aeronet protobuf headers links it | protobuf (libprotobuf, protoc), found with `find_package` and never fetched: it must be installed |
| `AERONET_PROTOBUF_RUNTIME` | `full` | protobuf runtime linked with `AERONET_ENABLE_PROTOBUF`: `full` (libprotobuf) or `lite` (libprotobuf-lite, for messages generated with `option optimize_for = LITE_RUNTIME`) | A program must not link both runtimes: `lite` cannot be combined with `AERONET_ENABLE_OPENTELEMETRY` |
| `AERONET_ENABLE_JWT` | ON only with OpenSSL + Glaze | JWS-profile JWT and JWKS support | Forced OFF unless both prerequisites are ON |

## Recipes

Small HTTP/1.1 server with no optional dependencies:

```bash
cmake -S . -B build -G Ninja -D CMAKE_BUILD_TYPE=Release \
  -D AERONET_BUILD_TESTS=OFF \
  -D AERONET_ENABLE_OPENSSL=OFF -D AERONET_ENABLE_HTTP2=OFF \
  -D AERONET_ENABLE_WEBSOCKET=OFF -D AERONET_ENABLE_HTTP_CLIENT=OFF \
  -D AERONET_ENABLE_ZLIB=OFF -D AERONET_ENABLE_ZSTD=OFF \
  -D AERONET_ENABLE_BROTLI=OFF -D AERONET_ENABLE_SPDLOG=OFF \
  -D AERONET_ENABLE_GLAZE=OFF -D AERONET_ENABLE_OPENTELEMETRY=OFF \
  -D AERONET_ENABLE_PROTOBUF=OFF
cmake --build build --parallel
```

Incremental development tree, for the edit / build / test loop:

```bash
cmake -S . -B build -G Ninja -D CMAKE_BUILD_TYPE=Debug -D AERONET_BUILD_SHARED=ON
cmake --build build --target http-core_test
ctest --test-dir build -R http-core --output-on-failure
```

With shared libraries, editing a library source relinks that library only, instead of every test executable linking it statically. A Debug build with tests also enables `AERONET_COMPACT_DEBUG_INFO`, which keeps the debug info in `.dwo` files next to the objects instead of copying it into each binary. A test executable then weighs a few MB instead of up to ~90 MB, and an edit rewrites a few MB instead of gigabytes. Keep a static tree for source-based coverage: `llvm-cov` only reads the binaries passed on its command line, so the library code must be linked into the test executables.

Unity build, for a tree that is built once and not edited - aeronet as a dependency, in a package or a CI image:
{ #unity-build }

```bash
cmake -S . -B build -G Ninja -D CMAKE_BUILD_TYPE=Release -D AERONET_BUILD_TESTS=OFF -D AERONET_UNITY_BUILD=ON
cmake --build build --parallel
```

The aeronet libraries are then compiled in batches of 16 sources, which parse the shared headers (standard library, glaze, spdlog...) once per batch instead of once per source: their compilation CPU time is divided by 4 to 6 in Release. Not for development: editing a source recompiles its whole batch, and `compile_commands.json` lists the batches instead of the sources. The tests and examples are never built as unity builds.

Static analysis with clang-tidy:
{ #static-analysis }

```bash
cmake -S . -B build -G Ninja -D CMAKE_BUILD_TYPE=Debug -D AERONET_BUILD_TESTS=ON \
  -D AERONET_ENABLE_CLANG_TIDY=ON -D AERONET_WARNINGS_AS_ERRORS=ON -D CLANG_TIDY=/usr/bin/clang-tidy-23
cmake --build build --parallel
```

The checks are those of the `.clang-tidy` file. `CXX_CLANG_TIDY` only analyzes translation units, and a check like `misc-include-cleaner` (missing and unused includes) reports on the main file only, so the headers get a target of their own, `aeronet-clang-tidy-headers` (part of `all`), which analyzes each `.hpp` as the main file of a clang-tidy run. The diagnostics of a header reached through a translation unit are dropped (`HeaderFilterRegex` in `.clang-tidy`), so that each finding is reported once, by the run of its header. A header is analyzed again only when it changes (a stamp file per header in `<build>/clang-tidy-headers`), not when a header it includes does. clang-tidy takes the compile flags of a header from the closest translation unit of `compile_commands.json`, which the option enables: the headers of the tests (`test_support` and the `test` directories) are therefore analyzed only with `AERONET_BUILD_TESTS=ON`. Without any test, `cmake --build build --target aeronet-clang-tidy-headers` is enough to check the headers of an edit.

Full development profile, matching the enabled-feature CI leg:

```bash
cmake -S . -B build -G Ninja -D CMAKE_BUILD_TYPE=Debug \
  -D AERONET_BUILD_TESTS=ON -D AERONET_ENABLE_TEST_HOOKS=ON \
  -D AERONET_ENABLE_ASAN=ON -D AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS=ON \
  -D AERONET_ENABLE_ASYNC_HANDLERS=ON -D AERONET_ENABLE_GLAZE=ON \
  -D AERONET_ENABLE_HTTP2=ON -D AERONET_ENABLE_WEBSOCKET=ON \
  -D AERONET_ENABLE_HTTP_CLIENT=ON -D AERONET_ENABLE_OPENSSL=ON \
  -D AERONET_ENABLE_ZLIB=ON -D AERONET_ENABLE_ZSTD=ON \
  -D AERONET_ENABLE_BROTLI=ON -D AERONET_ENABLE_SPDLOG=ON \
  -D AERONET_ENABLE_OPENTELEMETRY=ON -D AERONET_ENABLE_PROTOBUF=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The [installation guide](../getting-started/install.md) covers compilers, packages, installation, Conan, and vcpkg. Runtime controls are split into the [server](server-configuration.md) and [client](client-configuration.md) references.

## Checking what a binary was built with

The options above are compile-time, so a binary does not necessarily have the features of the build you had in mind. Two headers report it, both at no runtime cost:

- [`aeronet/features.hpp`](../../aeronet/tech/include/aeronet/features.hpp): one `constexpr bool` per feature, usable in `if constexpr` - `httpServerEnabled()`, `httpClientEnabled()`, `http2Enabled()`, `webSocketEnabled()`, `asyncHandlersEnabled()`, `openSslEnabled()`, `openTelemetryEnabled()`, `protobufEnabled()`, `spdLogEnabled()`, `zlibEnabled()`, `zstdEnabled()` and `brotliEnabled()`.
- [`aeronet/version.hpp`](../../aeronet/version/include/aeronet/version.hpp): `version()` is the aeronet semver, `fullVersionStringView()` a `constexpr` multi-line description of the build and `fullVersionWithRuntime()` the same string completed with the version of brotli loaded at runtime (brotli's version cannot be read at compile time). `aeronet-client.hpp` and `aeronet-server.hpp` include it.

```cpp
#include <aeronet/features.hpp>
#include <aeronet/version.hpp>

#include <iostream>

if constexpr (aeronet::http2Enabled()) {
  std::cout << "HTTP/2 is compiled in\n";
}
std::cout << aeronet::fullVersionWithRuntime() << '\n';
```

```text
aeronet 1.8.0-dev
  glaze: 8.1.0
  protobuf: 3.21.12
  tls: OpenSSL 3.0.13 30 Jan 2024
  logging: spdlog 1.15.0
  opentelemetry: 1.30.0
  features: http-server, http-client, http2, websocket, async-handlers
  compression: zlib-ng 2.2.4, zstd 1.5.6, brotli 1.1.0
```

A disabled dependency prints `disabled` (`tls: disabled`, `opentelemetry: disabled`...), `logging` is `in-house` without spdlog, `protobuf` is the version of the protobuf C++ runtime headers (`5.29.3` for protobuf v29.3) followed by ` (lite)` with `AERONET_PROTOBUF_RUNTIME=lite`, and the `features` line lists only the enabled flags among HTTP server, HTTP client, HTTP/2, WebSocket and async handlers (`none` when all are off). The dependencies that have their own line (TLS, glaze, protobuf, spdlog, OpenTelemetry, compression codecs) are not repeated there. Tests: [version_test.cpp](../../aeronet/version/test/version_test.cpp).

## Public headers and configuration files

Use `aeronet/aeronet.hpp` for the umbrella library API and `aeronet/aeronet-server.hpp` for server assembly. These headers define the configuration surface documented here:

- [`HttpServerConfig`](../../aeronet/objects/include/aeronet/http-server-config.hpp), plus TLS, HTTP/2, compression, decompression, probe, telemetry, access-log, and static-file sub-configurations.
- [`HttpClientConfig`](../../aeronet/client/include/aeronet/http-client-config.hpp) and [`RetryConfig`](../../aeronet/client/include/aeronet/retry-config.hpp).
- [`RouterConfig`](../../aeronet/http/include/aeronet/router-config.hpp) for trailing slash and default CORS policy.

When Glaze is enabled, `aeronet-config-dump` can emit the complete serializable server/router schema as JSON or YAML, and the client one with `--client`. Callback fields in `StaticFileConfig` are intentionally code-only and are not serializable (see [Field names](../modules/json-and-config-files.md#field-names)).

Two configuration headers are intentionally not application configuration objects:

- [`aeronet-config.hpp`](../../aeronet/server/include/aeronet/aeronet-config.hpp) holds the Glaze metadata of the server configuration objects, kept out of their headers to save compile time: include it to serialize them with Glaze yourself (see [Serialize configuration objects yourself](../modules/json-and-config-files.md#serialize-configuration-objects-yourself)).
- [`compiler-config.hpp`](../../aeronet/tech/include/aeronet/compiler-config.hpp) selects portability attributes such as `AERONET_RESTRICT` and `AERONET_ALWAYS_INLINE` for the active compiler. Applications should not define or override these macros; select the compiler/toolchain through CMake.
