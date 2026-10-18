# Getting started

This section takes you from a checkout to a running server, then points to the next guide for each common task.

## Prerequisites

aeronet requires a C++23 compiler and CMake 3.28 or newer. Linux is the primary platform; macOS and Windows are also supported through the same public API. The complete toolchain matrix and package-manager instructions are in [Installation and build](install.md).

## Choose the features you need

The core build is intentionally modular. Enable optional capabilities at CMake configure time:

- `AERONET_ENABLE_OPENSSL` for TLS and HTTPS.
- `AERONET_ENABLE_HTTP2` for HTTP/2, including ALPN and h2c.
- `AERONET_ENABLE_WEBSOCKET` for RFC 6455 endpoints.
- `AERONET_ENABLE_ASYNC_HANDLERS` for coroutine handlers.
- `AERONET_ENABLE_HTTP_CLIENT` for the synchronous client and connection pool.
- `AERONET_ENABLE_HTTP_SERVER` for the `HttpServer`
- `AERONET_ENABLE_OPENTELEMETRY` for traces/metrics, `AERONET_ENABLE_GLAZE` for JSON/YAML configuration, and `AERONET_ENABLE_JWT` for JWT/JWKS. JWT is available only when both OpenSSL and Glaze are enabled.

Set `AERONET_ENABLE_HTTP_CLIENT`to `OFF` if you need only the server, or `AERONET_ENABLE_HTTP_SERVER` to `OFF` if you need only the client to minimize the code to be compiled.

Note that it is not possible to exclude both.

The [build configuration reference](../reference/configuration.md) explains every feature gate and dependency. The [server](../reference/server-configuration.md) and [client](../reference/client-configuration.md) references cover runtime controls.

## Feature overview

What is implemented, what is optional (*flag*: enabled by a CMake option), and where each feature is documented.

| Area | What you get | Read more |
| --- | --- | --- |
| HTTP/1.1 parsing | Request line, headers, chunked bodies, pipelining, deterministic and security-minded duplicate header policy, incoming request trailers | [Requests](../guides/requests.md), [HTTP/1.1 conformance](../reference/http-conformance.md) |
| Routing | Exact paths, path parameters and wildcards, method allow-lists, streaming and fixed handlers coexisting on one path (for instance streaming `GET`, fixed `POST`), heterogeneous path lookups (`std::string`, `std::string_view`, `const char*`) | [Routing](../guides/routing.md) |
| Connections and limits | Keep-alive, maximum header and body sizes, maximum requests per connection, idle timeout | [Servers and lifecycle](../guides/server-lifecycle.md#connections-and-keep-alive), [Server configuration](../reference/server-configuration.md) |
| Middleware | Global and per-route request / response hooks (streaming-aware), rate limiting, CORS | [Middleware](../guides/middleware.md), [CORS](../guides/cors.md) |
| Compression | gzip, deflate, zstd and brotli; `q`-value negotiation, size threshold, per-response opt-out, direct compression | [Compression](../guides/compression.md) |
| Inbound decompression | Multi-layer codings, safety guards, header removal, streaming inflaters once `Content-Length` exceeds a configured threshold | [Compression](../guides/compression.md#request-body-decompression) |
| Streaming and files | Chunked responses, outgoing trailers, `sendfile` for plain sockets, range and conditional requests | [Streaming responses](../guides/streaming-responses.md), [Static files](../guides/static-files.md) |
| Async handlers | Coroutine handlers and background work off the event loop | [Async handlers](../guides/async-handlers.md) |
| TLS (*flag*) | ALPN, mTLS, SNI, session tickets, cached OCSP stapling, CRL and revocation hooks, debug key logs, kTLS, metrics | [TLS](../protocols/tls.md) |
| HTTP/2 (*flag*) | RFC 9113, HPACK, ALPN `h2`, h2c prior knowledge, stream multiplexing, incoming trailers | [HTTP/2](../protocols/http2.md) |
| WebSocket (*flag*) | RFC 6455, text / binary frames, ping / pong, close handshake | [WebSocket](../protocols/websocket.md) |
| HTTP client (*flag*) | Synchronous HTTP/1.1 and HTTP/2, keep-alive pool, redirects, automatic (de)compression, HTTPS, streamed response bodies | [HTTP client](../modules/http-client.md) |
| JWT (*flag*) | RFC 7519 sign / verify, HS / RS / ES / PS / EdDSA, JWK / JWKS by `kid`, rejects `alg: none` | [JWT](../modules/jwt.md) |
| Observability | Logging (spdlog optional, ISO 8601 UTC timestamps), per-request metrics callback (alpha), OpenTelemetry traces and metrics (experimental, *flag*), outbound statistics (bytes queued, immediate vs flush writes, high-water marks) | [Observability](../operations/observability.md), [Performance](../operations/performance.md) |
| Event loop | Edge-triggered epoll loop, one thread per `SingleHttpServer`, `writev` scatter-gather for header and body | [Performance and tuning](../operations/performance.md) |
| Scaling | `SO_REUSEPORT` multi-reactor scaling, `MultiHttpServer` orchestrating N reactors on N threads | [Servers and lifecycle](../guides/server-lifecycle.md#threading-model) |
| Lifecycle | RAII construction (listening after the constructor, ephemeral port resolved immediately), blocking `run()`, non-blocking `start()` and `startDetached()` (returns an `AsyncHandle`), restarts, move semantics, runtime configuration and router updates | [Servers and lifecycle](../guides/server-lifecycle.md) |
| Graceful shutdown | `beginDrain(maxWait)` stops accepting, closes keep-alive connections after the current responses, and can force-close stragglers; optional built-in `SIGINT` / `SIGTERM` handler | [Servers and lifecycle](../guides/server-lifecycle.md#stopping) |
| Configuration | Fluent `HttpServerConfig` setters (`withPort()`, ...), JSON / YAML files | [Server configuration](../reference/server-configuration.md), [JSON and config files](../modules/json-and-config-files.md) |
| Kubernetes | Built-in liveness, readiness and startup probes | [Health probes](../operations/health-probes.md), [Kubernetes](../operations/kubernetes.md) |

The tests behind these features are summarized in the [test coverage matrix](../development/testing.md#test-coverage-matrix). The exhaustive matrix of HTTP/1.1 behavior is the [HTTP/1.1 conformance](../reference/http-conformance.md) reference.

## Recommended learning path

1. Follow [Your first server](first-server.md) to run a route on port 8080.
2. Read [Routing and requests](../guides/routing.md) to add paths, parameters, and request handling.
3. Read [Requests](../guides/requests.md) and [Responses](../guides/responses.md), then [Middleware](../guides/middleware.md), [Streaming responses](../guides/streaming-responses.md), or [Static files](../guides/static-files.md) as needed, and [Async handlers](../guides/async-handlers.md) when handlers make blocking calls.
4. Add a protocol or module when needed: [TLS](../protocols/tls.md), [HTTP/2](../protocols/http2.md), [WebSocket](../protocols/websocket.md), the [HTTP client](../modules/http-client.md), or [JWT](../modules/jwt.md).

!!! tip
    The repository's [examples directory](https://github.com/sjanel/aeronet/tree/main/examples) is the best source for complete, buildable programs. The guides focus on how the pieces fit together and link to those executable examples.
