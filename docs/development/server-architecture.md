# Server architecture

This page describes how a `SingleHttpServer` is organized internally, for contributors changing the server. It is not needed to use aeronet: the public API is documented in the guides.

A `SingleHttpServer` is one event loop (epoll, kqueue or WSAPoll) serving its connections on the thread that runs it. `MultiHttpServer` runs several of them, one per thread. Everything below happens on the event-loop thread, except the updates posted from other threads (configuration, router, callbacks) and the deferred work of async handlers, which reach the event loop through queues protected by a mutex.

## Parts of a server

[`SingleHttpServer`](../../aeronet/server/include/aeronet/single-http-server.hpp) keeps the lifecycle (start, stop, drain), the listening socket, the dispatch of the polled events, the updates posted from other threads, and the public API. It owns an [`internal::ServerContext`](../../aeronet/server/include/aeronet/internal/server-context.hpp): the state that the parts of the server share (configuration, router, connection storage, event loop, lifecycle, telemetry, statistics, user callbacks, compression state, shared scratch buffers), and the components serving the connections.

| Component | Sources | Responsibility |
| --- | --- | --- |
| `SingleHttpServer` | [single-http-server.cpp](../../aeronet/server/src/single-http-server.cpp), [http-server-lifecycle.cpp](../../aeronet/server/src/http-server-lifecycle.cpp), [server-updates.cpp](../../aeronet/server/src/server-updates.cpp), [http2-connections.cpp](../../aeronet/server/src/http2-connections.cpp) | Event loop and maintenance tick, accept, read loop of a connection, selection of its protocol (HTTP/1.1, HTTP/2 with prior knowledge or ALPN, protocol handler), updates, lifecycle |
| `ConnectionIo` | [connection-io.cpp](../../aeronet/server/src/connection-io.cpp) | Output of the connections (queued, batched for pipelined requests, gathered, `MSG_ZEROCOPY`, files with `sendfile()` or user-space TLS), writable interest, input buffers lent to the reads, input deferred to the end of the loop iteration, close (lingering or not) and release |
| `ConnectionMaintenance` | [connection-maintenance.cpp](../../aeronet/server/src/connection-maintenance.cpp) | Deadline queue (keep-alive, lingering close, WebSocket and zerocopy timeouts), header read, body read and per-route request timers, periodic sweep of the connections |
| `Http1Pipeline` | [http1-pipeline.cpp](../../aeronet/server/src/http1-pipeline.cpp), [http-parser.cpp](../../aeronet/server/src/http-parser.cpp), [async-handlers.cpp](../../aeronet/server/src/async-handlers.cpp), [websocket-connections.cpp](../../aeronet/server/src/websocket-connections.cpp) | HTTP/1.1 framing (head, bodies, trailers, `Expect`, request decompression), protocol-specific answers (errors, HTTPS redirect, `OPTIONS`, `TRACE`, `CONNECT`, WebSocket upgrade), streaming and async handlers |
| `TlsAcceptor` | [tls-acceptor.cpp](../../aeronet/server/src/tls-acceptor.cpp) | TLS context (rebuilt on updates and when its files change), handshake admission (concurrency and rate limits), TLS transport of the accepted connections, report of the handshake outcomes |
| `RequestPipeline` | [http-request-dispatch.cpp](../../aeronet/server/src/http-request-dispatch.cpp) | Steps shared by HTTP/1.1 and HTTP/2 once a request is routed: request middleware, CORS, handler call, trailing slash redirects, response middleware |
| `Http2ProtocolHandler` | [http2-protocol-handler.cpp](../../aeronet/server/src/http2-protocol-handler.cpp) | HTTP/2 connection (one per connection): frames, streams, flow control, and the requests of its streams through the `RequestPipeline` |
| `TunnelManager` | [tunnel-manager.cpp](../../aeronet/server/src/tunnel-manager.cpp) | `CONNECT` tunnels of both protocols: background name resolution, relay with backpressure, teardown. Created by the first `CONNECT` request of a run |

The connection storage ([connection-storage.hpp](../../aeronet/server/include/aeronet/internal/connection-storage.hpp)) holds a `ConnectionState` per connection ([connection-state.hpp](../../aeronet/server/include/aeronet/connection-state.hpp)): its transport, input and output buffers, current HTTP/1.1 request, protocol handler, timers and flags.

## How the parts reach each other

The components are members of the `ServerContext` held by value, and each of them keeps a single pointer back to it: no virtual call nor `std::function` sits between them, so splitting the server costs nothing on the hot path. A component calls another one through the context (`_pCtx->io.flushOutbound(cnxIt)`), and reads the shared state the same way (`_pCtx->config`).

A server can be moved (only while it does not run): the move operations of `ServerContext` point the components of the destination to its own context. A new component must be added to `ServerContext::bindComponents()` and to these move operations. The tests `HttpServerMove.ServesAfterMovedFromServerIsDestroyed` ([http-server-lifecycle_test.cpp](../../tests/http-server-lifecycle_test.cpp)) and `HttpTlsMoveAlpn.AcceptsTlsAfterMovedFromServerIsDestroyed` ([http-tls-handshake_test.cpp](../../tests/http-tls-handshake_test.cpp)) serve requests once the moved-from server is destroyed, so that a component still pointing to it reads freed memory (reported by the sanitizer builds).

A call into another source file costs a few nanoseconds that add up on the per-event and per-request paths: the bookkeeping that runs for each request (timers, writable interest, input buffer lending) is defined inline in the component headers, and the steps a hot function was split into are `AERONET_ALWAYS_INLINE` (`Http1Pipeline::readHead()`, `callHandlers()`...). Check such changes with [http1-pipeline_bench.cpp](../../benchmarks/internal/http1-pipeline_bench.cpp) (request path) and the scripted servers under `wrk` (event path, see [Benchmarks](../BENCHMARKS.md)), in Release.

The objects working on the connections on behalf of the server hold a `ServerContext&`: the `TunnelManager`, the HTTP/1.1 transport of the streaming response writers ([http1-writer-transport.hpp](../../aeronet/server/src/http1-writer-transport.hpp)) and the tunnel bridge of the HTTP/2 handlers. They only exist while the server runs, so the server cannot move under them.

## Life of an HTTP/1.1 request

1. `SingleHttpServer::eventLoop()` polls the events, and hands those of a connection to `handleConnectionEvent()`, which refreshes its keep-alive deadline (`ConnectionMaintenance`) and calls the read and write handlers.
2. `handleReadableClient()` reads into the input buffer lent by `ConnectionIo` (the one of the previous read, still in the CPU caches), within the fairness budget, then `processConnectionInput()` gives the input to the protocol of the connection.
3. `Http1Pipeline::processRequests()` serves the requests of the input buffer: `readHead()` parses the head and runs the checks that do not depend on the route, then the route is matched, its limits applied (`applyRouteLimits()`), the `Expect` header answered, the body decoded, the special methods answered, and `callHandlers()` runs the request middleware and the handler through the `RequestPipeline`.
4. `Http1Pipeline::finalizeAndSendResponse()` builds the response, and `ConnectionIo::queueData()` writes it. The responses to pipelined requests are batched and written together once the last request of the input is served.
5. The maintenance tick (`runMaintenance()`) closes the connections whose deadline elapsed, and sweeps those that need a closer look (pending output, timers, HTTP/2 streams, drain).

Changes to the steps shared by both protocols belong in the `RequestPipeline`, with a scenario in [http-protocol-parity_test.cpp](../../tests/http-protocol-parity_test.cpp) checking that HTTP/1.1 and HTTP/2 answer alike.

## Invariants

- **Connection iterators**: inserting a connection (the upstream of a `CONNECT` tunnel) may move the entries of the connection storage. Code that may insert one re-finds the iterator of its connection by its file descriptor afterwards, and keeps the `ConnectionState` pointer, which is stable.
- **Input buffers**: an empty input buffer holds no memory, an idle connection keeping none. Request views point into the input buffer, so pending input (a partial request, a request held by an async handler or waiting for its `CONNECT` target) stays where it is until served.
- **Edge-triggered polling**: the kernel reports new data only, so input left unread (fairness budget, output backpressure, records read ahead by TLS) is resumed explicitly through `ConnectionIo::deferInput()` at the end of the loop iteration.
- **Threads**: only the event-loop thread touches the connections and the components. Other threads post updates (applied at the start of the next loop iteration) and the completion of the deferred work of async handlers (through the `AsyncResumePoster` of their connection, a function pointer to `ServerContext::PostAsyncResume()` with the connection it resumes), both waking the event loop up.
