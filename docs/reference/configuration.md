# Build configuration

aeronet is deliberately modular. CMake options select code and dependencies at configure time; runtime configuration only enables behavior that was compiled in. Options that use `PROJECT_IS_TOP_LEVEL` default to `ON` from an aeronet checkout and `OFF` when aeronet is brought in with `FetchContent` or another package manager.

## Build products and developer tools

| Option | Top-level default | Meaning |
| --- | --- | --- |
| `AERONET_INSTALL` | ON | Generate install rules and the CMake package export. |
| `AERONET_BUILD_EXAMPLES` | ON | Build the programs in `examples/`; OFF for a dependency. |
| `AERONET_BUILD_TESTS` | ON | Build unit and integration tests; OFF for a dependency. |
| `AERONET_BUILD_BENCHMARKS` | ON except Debug | Build benchmarks and their selected comparison backends. |
| `AERONET_BUILD_SHARED` | OFF | Build shared libraries instead of static libraries. |
| `AERONET_BUILD_MODULES` | OFF | Experimental C++ module build. |
| `AERONET_ENABLE_CCACHE` | top-level | Use `ccache` when it is available. |
| `AERONET_ENABLE_TEST_HOOKS` | non-Release tests | Enable transport hooks used by tests. Do not enable in production builds. |
| `AERONET_ENABLE_ASAN` | OFF | Enable AddressSanitizer, UndefinedBehaviorSanitizer, and float-divide checks. |
| `AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS` | OFF | Enable aeronet's additional runtime memory checks. |
| `AERONET_ENABLE_CLANG_TIDY` | OFF | Run clang-tidy during the build. |
| `AERONET_ENABLE_WARNINGS` | top-level | Enable the project warning set. |
| `AERONET_WARNINGS_AS_ERRORS` | OFF | Promote warnings to errors. |
| `AERONET_SPDLOG_USE_STD_FORMAT` | ON | Use `std::format` rather than fmt through spdlog. |

## Protocol and integration features

| Option | Default | Provides | Dependency or relationship |
| --- | --- | --- | --- |
| `AERONET_ENABLE_OPENSSL` | top-level | HTTPS, TLS client/server support, ALPN, mTLS, session tickets | OpenSSL |
| `AERONET_ENABLE_HTTP2` | ON | HTTP/2, HPACK, h2c, and ALPN negotiation | Pair with OpenSSL for HTTP/2 over TLS |
| `AERONET_ENABLE_WEBSOCKET` | ON | RFC 6455 endpoints | `AERONET_ENABLE_ZLIB` additionally enables permessage-deflate |
| `AERONET_ENABLE_HTTP_CLIENT` | ON | Synchronous `HttpClient`, pool, proxy, cache, retry | OpenSSL for HTTPS; HTTP/2 option for h2 client |
| `AERONET_ENABLE_ASYNC_HANDLERS` | ON | Coroutine routing APIs | No external dependency |
| `AERONET_ENABLE_RESPONSE_CACHE` | ON | Per-route bounded in-memory server response cache | No external dependency |
| `AERONET_ENABLE_ZLIB` | top-level | gzip and deflate encoders/decoders | zlib-ng is preferred by default |
| `AERONET_ENABLE_ZLIBNG` | ON | Use zlib-ng in place of classic zlib | Meaningful only with zlib enabled |
| `AERONET_ENABLE_ZSTD` | top-level | zstd encoders/decoders | zstd |
| `AERONET_ENABLE_BROTLI` | top-level | Brotli encoders/decoders | Brotli |
| `AERONET_ENABLE_SPDLOG` | top-level | spdlog integration | spdlog |
| `AERONET_ENABLE_OPENTELEMETRY` | top-level | OpenTelemetry traces/metrics exporter | OpenTelemetry C++, curl, protobuf/protoc |
| `AERONET_ENABLE_GLAZE` | top-level | JSON/YAML config loading and serialization helpers | glaze |
| `AERONET_ENABLE_JWT` | ON only with OpenSSL + Glaze | JWS-profile JWT and JWKS support | Forced OFF unless both prerequisites are ON |

## Recipes

Small HTTP/1.1 server with no optional dependencies:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DAERONET_BUILD_TESTS=OFF \
  -DAERONET_ENABLE_OPENSSL=OFF -DAERONET_ENABLE_HTTP2=OFF \
  -DAERONET_ENABLE_WEBSOCKET=OFF -DAERONET_ENABLE_HTTP_CLIENT=OFF \
  -DAERONET_ENABLE_RESPONSE_CACHE=OFF \
  -DAERONET_ENABLE_ZLIB=OFF -DAERONET_ENABLE_ZSTD=OFF \
  -DAERONET_ENABLE_BROTLI=OFF -DAERONET_ENABLE_SPDLOG=OFF \
  -DAERONET_ENABLE_GLAZE=OFF -DAERONET_ENABLE_OPENTELEMETRY=OFF
cmake --build build --parallel
```

Full development profile, matching the enabled-feature CI leg:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DAERONET_BUILD_TESTS=ON -DAERONET_ENABLE_TEST_HOOKS=ON \
  -DAERONET_ENABLE_ASAN=ON -DAERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS=ON \
  -DAERONET_ENABLE_ASYNC_HANDLERS=ON -DAERONET_ENABLE_GLAZE=ON \
  -DAERONET_ENABLE_HTTP2=ON -DAERONET_ENABLE_WEBSOCKET=ON \
  -DAERONET_ENABLE_HTTP_CLIENT=ON -DAERONET_ENABLE_OPENSSL=ON \
  -DAERONET_ENABLE_RESPONSE_CACHE=ON \
  -DAERONET_ENABLE_ZLIB=ON -DAERONET_ENABLE_ZSTD=ON \
  -DAERONET_ENABLE_BROTLI=ON -DAERONET_ENABLE_SPDLOG=ON \
  -DAERONET_ENABLE_OPENTELEMETRY=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The [installation guide](../INSTALL.md) covers compilers, packages, installation, Conan, and vcpkg. Runtime controls are split into the [server](server-configuration.md) and [client](client-configuration.md) references.

## Public headers and configuration files

Use `aeronet/aeronet.hpp` for the umbrella library API and `aeronet/aeronet-server.hpp` for server assembly. These headers define the configuration surface documented here:

- [`HttpServerConfig`](../../aeronet/objects/include/aeronet/http-server-config.hpp), plus TLS, HTTP/2, compression, decompression, probe, telemetry, access-log, and static-file sub-configurations.
- [`HttpClientConfig`](../../aeronet/client/include/aeronet/http-client-config.hpp) and [`RetryConfig`](../../aeronet/client/include/aeronet/retry-config.hpp).
- [`RouterConfig`](../../aeronet/http/include/aeronet/router-config.hpp) for trailing slash and default CORS policy.

When Glaze is enabled, `aeronet-config-dump` can emit the complete serializable server/router schema as JSON or YAML. Callback fields in `StaticFileConfig` are intentionally code-only and are not serializable.

Two configuration headers are intentionally not application configuration objects:

- [`aeronet-config.hpp`](../../aeronet/http/include/aeronet/aeronet-config.hpp) contains the internal combined server/router representation used by the config loader. Use a server file-path constructor or the public `LoadServerConfig` path instead of depending on `TopLevelConfig` or its parser helpers.
- [`compiler-config.hpp`](../../aeronet/tech/include/aeronet/compiler-config.hpp) selects portability attributes such as `AERONET_RESTRICT` and `AERONET_ALWAYS_INLINE` for the active compiler. Applications should not define or override these macros; select the compiler/toolchain through CMake.
