# Configuration and optional modules

`HttpServerConfig` centralizes listener, protocol, transport, and runtime behavior. It uses fluent `with...()` setters so a configuration can be assembled before the server takes ownership of it.

## Build-time feature gates

Enable only the modules your application uses. The most common CMake options are:

| Option | Enables |
| --- | --- |
| `AERONET_ENABLE_OPENSSL` | TLS and HTTPS support |
| `AERONET_ENABLE_HTTP2` | HTTP/2, HPACK, ALPN, and h2c support |
| `AERONET_ENABLE_WEBSOCKET` | WebSocket endpoints |
| `AERONET_ENABLE_ASYNC_HANDLERS` | Coroutine handler APIs |
| `AERONET_ENABLE_ZLIB`, `AERONET_ENABLE_ZSTD`, `AERONET_ENABLE_BROTLI` | Compression codecs |
| `AERONET_ENABLE_OPENTELEMETRY` | OpenTelemetry instrumentation |
| `AERONET_ENABLE_GLAZE` | JSON serialization helpers |

The complete option list, defaults, and platform notes are in [Installation and build](../INSTALL.md#cmake-options).

## Runtime configuration

Create the server from a fully configured `HttpServerConfig` and `Router`. Some fields are fixed once a listener exists, while operational limits, timeouts, compression, headers, and TLS settings have documented update behavior.

Use the [feature reference](../FEATURES.md) as the source of truth for each module's settings. In particular, check the relevant section before changing TLS, HTTP/2 flow-control, compression, static-file, or observability configuration in production.

## Public headers

Applications normally include the public umbrella headers exposed by the installed package. The repository's [aeronet/aeronet.hpp](https://github.com/sjanel/aeronet/blob/main/aeronet/main/include/aeronet/aeronet.hpp) and [aeronet/aeronet-server.hpp](https://github.com/sjanel/aeronet/blob/main/aeronet/server/include/aeronet/aeronet-server.hpp) headers show the available API surface.
