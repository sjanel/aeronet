---
title: aeronet documentation
description: Documentation for the high-performance, modular C++23 HTTP, HTTP/2, and WebSocket library.
---

# aeronet

**aeronet** is a modular C++23 library for building fast HTTP/1.1, HTTP/2, and WebSocket servers and clients. It is designed for predictable performance, explicit control, and opt-in features - without hidden global state or macros.

[Get started](getting-started/overview.md){ .md-button .md-button--primary }
[View on GitHub](https://github.com/sjanel/aeronet){ .md-button }

## What aeronet provides

- HTTP/1.1 server and client support, with keep-alive, streaming, trailers, compression, and static-file helpers.
- Optional TLS, HTTP/2, WebSocket, asynchronous handlers, OpenTelemetry, Glaze, and JWT modules.
- A portable event-loop abstraction for Linux, macOS, and Windows, while preserving Linux-focused performance features where available.
- A CMake-first integration model: enable only the modules your application needs.

## Start with the path that matches your task

| I want to... | Start here |
| --- | --- |
| Build and consume the library | [Installation and build](INSTALL.md) |
| Select CMake features or tune runtime settings | [Build configuration](reference/configuration.md), [server configuration](reference/server-configuration.md), or [client configuration](reference/client-configuration.md) |
| Run a minimal HTTP server | [Your first server](getting-started/first-server.md) |
| Define routes and process requests | [Routing and requests](guides/routing.md) |
| Stream a response or serve a file | [Bodies, streaming, and static files](guides/bodies-streaming-and-files.md) |
| Enable TLS or HTTP/2 | [TLS and HTTP/2](protocols/tls-and-http2.md) |
| Add WebSocket support | [WebSocket](protocols/websocket.md) |
| Configure logging and telemetry | [Observability and logging](operations/observability.md) |
| Assemble a public, TLS, Kubernetes, or upstream-client profile | [Production deployment patterns](guides/production-configuration.md) |

## Documentation status

This site is the primary navigation layer for aeronet documentation. Focused guides explain common workflows, and the feature reference retains protocol-level behavior and edge cases. Build, server, and client configuration have dedicated references derived from the public configuration headers.

## Project links

- [Source code and issues](https://github.com/sjanel/aeronet)
- [Latest releases](https://github.com/sjanel/aeronet/releases)
- [Live benchmark dashboards](https://sjanel.github.io/aeronet/benchmarks/)
- [Project roadmap](ROADMAP.md)
