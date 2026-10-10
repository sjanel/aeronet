# Performance and tuning

aeronet is designed to serve requests with few system calls, few allocations, and no copies of request or response data. The defaults suit most deployments; this page explains what the server does on the hot path and which settings change its trade-offs.

## How a request is served

- **One event loop per thread, nothing shared.** Each worker of a `MultiHttpServer` has its own listening socket (`SO_REUSEPORT`), event loop, connections, and copy of the router: no lock on the request path. See [Servers and lifecycle](../guides/server-lifecycle.md#threading-model).
- **Parsing without copies.** The request is parsed in place in the connection buffer: path, headers, parameters, and body are views into it (see [Requests](../guides/requests.md#lifetime-of-request-data)). Duplicate headers are merged in place, without allocation.
- **One read per request.** The edge-triggered read loop stops on a short read, which proves the kernel queue empty, instead of reading again until `EAGAIN`. Plaintext sockets use `recv()` / `send()` / `sendmsg()`, which skip the file layer checks of `read()` / `write()`.
- **One buffer per response.** Status line, headers, and an inline body are laid out in a single buffer, as they are sent; a large owned body is sent from its own buffer. Head and body go out in a single gather write (`writev`, `WSASend` on Windows).
- **Few segments.** On Linux, with `TCP_NODELAY`, responses are written under `TCP_CORK`, so that a response is not split in partial segments.
- **Reuse.** Closed connection objects and their buffers are kept for reuse (`maxCachedConnections`), and buffers keep their capacity across the requests of a connection.
- **Cheap timers.** Keep-alive deadlines live in a min-heap updated once per `keepAliveTimeout` for an active connection, not once per event; timeouts are driven by a timer, independently of traffic.

Large payloads avoid user-space copies: files are sent with `sendfile()` (`TransmitFile` on Windows), large buffers with `MSG_ZEROCOPY` on Linux, and TLS payloads through [kernel TLS](../protocols/tls.md#kernel-tls-ktls) when available. Responses can be [compressed while they are written](../guides/compression.md#direct-compression).

## Settings

| Setting | Default | Trade-off |
| --- | --- | --- |
| `nbThreads` | hardware threads | Event loops of a `MultiHttpServer`. More threads than cores rarely helps; fewer leave room for other processes or `deferWork()` threads. |
| `maxPerEventReadBytes` | 128 KiB | Fairness budget of a connection per readable event, reads and writes included. Beyond it, other ready connections are served first. Raise it for few connections with large bodies, lower it for many concurrent clients. |
| `minReadChunkBytes` | 4 KiB | Smallest follow-up read within one event. |
| `maxAcceptBatchSize` | 64 | Connections accepted per loop iteration, so that bursts do not starve open connections. `0` is unlimited. |
| `pollInterval`, `pollIntervalMinFactor`, `pollIntervalMaxFactor` | 500 ms, 1.0, 1.0 | Longest sleep of an idle loop: bounds how fast `stop()` and predicates are noticed. Timeouts do not depend on it. Factors let saturated loops poll faster and idle loops back off. |
| `tcpNoDelay` | `Auto` | `TCP_NODELAY` on TLS and HTTP/2 connections, where small records would otherwise stall on Nagle's algorithm combined with delayed acknowledgments. `Enabled` or `Disabled` force it. |
| `zerocopyMode`, `zerocopyMinBytes` | `Opportunistic`, 128 KiB | `MSG_ZEROCOPY` for payloads of at least `zerocopyMinBytes`, on non-loopback connections. Below about 10 KiB, pinning pages costs more than copying. |
| `maxZerocopyPendingBytes` | 4 MiB | Payloads a connection may keep waiting for zerocopy completions; beyond it, that connection falls back to copies. |
| `maxOutboundBufferBytes` | 4 MiB | Response data queued for a connection that reads slowly. Beyond it, an HTTP/1.1 connection is closed once flushed, and HTTP/2 stops reading frames. |
| `maxCachedConnections` | 10 | Closed connection objects kept for reuse. |
| `minCapturedBodySize` | 1 KiB | Owned response bodies smaller than this are copied next to the head. |

`MSG_ZEROCOPY` buffers stay alive until the kernel reports their transmission, including when the connection closes: a graceful close waits for the pending sends, and a forced close resets the connection, so that the kernel drops them before their buffers are freed. `Opportunistic` mode skips loopback connections, where zerocopy brings nothing, so that local benchmarks stay representative.

On Linux, a write to a connection reset by the peer fails with `EPIPE` instead of raising `SIGPIPE`, whose default action kills the process: plaintext writes use `MSG_NOSIGNAL`, and event-loop threads block `SIGPIPE` while they run, for the writes that cannot pass a flag (`sendfile()`, OpenSSL). The process-wide `SIGPIPE` disposition is left untouched; threads created from an event-loop thread inherit the blocked signal.

## Writing fast handlers

- Build responses with `req.makeResponse()`, and fill them in order: status, headers, body, trailers (see [Responses](../guides/responses.md)).
- Move large bodies into responses (`body(std::move(buffer))`) instead of copying them, and serve files with `file()` or the [static file handler](../guides/static-files.md).
- Prefer `headerAddLine()` to `header()` when the header cannot be present yet.
- Never block the event loop: move blocking work to `deferWork()` in an [async handler](../guides/async-handlers.md#performance), whose measurements show the cost of each approach.
- Restrict compression to compressible content types (`contentTypeAllowList`), and pick codec levels that fit your CPU budget.

## Measuring

`server.stats()` returns per-server counters; these describe output backpressure:

| `ServerStats` field | Meaning |
| --- | --- |
| `totalBytesQueued` | Response bytes accepted for sending. |
| `totalBytesWrittenImmediate`, `totalBytesWrittenFlush` | Bytes written at once, and bytes written later when the socket became writable. |
| `deferredWriteEvents`, `flushCycles` | How often output had to wait for the socket. |
| `maxConnectionOutboundBuffer` | Largest output queue of a connection: compare it with `maxOutboundBufferBytes`. |

`MultiHttpServer::stats()` returns the counters of each worker (`per`) and their sum (`total`). The [benchmarks page](../BENCHMARKS.md) describes the benchmark suites, how CI runs them, and how to profile the server.
