# Getting started

This section takes you from a checkout to a running server, then points to the next guide for each common task.

## Prerequisites

aeronet requires a C++23 compiler and CMake 3.28 or newer. Linux is the primary platform; macOS and Windows are also supported through the same public API. The complete toolchain matrix and package-manager instructions are in [Installation and build](../INSTALL.md).

## Choose the features you need

The core build is intentionally modular. Enable optional capabilities at CMake configure time:

- `AERONET_ENABLE_OPENSSL` for TLS and HTTPS.
- `AERONET_ENABLE_HTTP2` for HTTP/2, including ALPN and h2c.
- `AERONET_ENABLE_WEBSOCKET` for RFC 6455 endpoints.
- `AERONET_ENABLE_ASYNC_HANDLERS` for coroutine handlers.
- `AERONET_ENABLE_OPENTELEMETRY`, `AERONET_ENABLE_GLAZE`, and the JWT option when your application needs them.

The [configuration reference](../reference/configuration.md) explains the feature gates and their dependencies.

## Recommended learning path

1. Follow [Your first server](first-server.md) to run a route on port 8080.
2. Read [Routing and requests](../guides/routing.md) to add paths, parameters, and request handling.
3. Choose [Middleware and responses](../guides/middleware-and-responses.md) or [Bodies, streaming, and static files](../guides/bodies-streaming-and-files.md) according to the response model you need.
4. Add a protocol module when needed: [TLS and HTTP/2](../protocols/tls-and-http2.md), [WebSocket](../protocols/websocket.md), or the [HTTP client and JWT](../protocols/client-and-jwt.md) guide.

!!! tip
    The repository's [examples directory](https://github.com/sjanel/aeronet/tree/main/examples) is the best source for complete, buildable programs. The guides focus on how the pieces fit together and link to those executable examples.
