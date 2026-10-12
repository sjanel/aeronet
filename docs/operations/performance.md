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

Large payloads avoid user-space copies: files are sent with `sendfile()` (`TransmitFile` on Windows), large buffers with [`MSG_ZEROCOPY`](#msg_zerocopy) on Linux, and TLS payloads through [kernel TLS](../protocols/tls.md#kernel-tls-ktls) when available. Responses can be [compressed while they are written](../guides/compression.md#direct-compression).

## Settings

| Setting | Default | Trade-off |
| --- | --- | --- |
| `nbThreads` | hardware threads | Event loops of a `MultiHttpServer`. More threads than cores rarely helps; fewer leave room for other processes or `deferWork()` threads. |
| `maxPerEventReadBytes` | 128 KiB | Fairness budget of a connection per readable event, reads and writes included. Beyond it, other ready connections are served first. Raise it for few connections with large bodies, lower it for many concurrent clients. |
| `minReadChunkBytes` | 4 KiB | Smallest follow-up read within one event. |
| `maxAcceptBatchSize` | 64 | Connections accepted per loop iteration, so that bursts do not starve open connections. `0` is unlimited. |
| `pollInterval`, `pollIntervalMinFactor`, `pollIntervalMaxFactor` | 500 ms, 1.0, 1.0 | Longest sleep of an idle loop: bounds how fast `stop()` and predicates are noticed. Timeouts do not depend on it. Factors let saturated loops poll faster and idle loops back off. |
| `tcpNoDelay` | `Auto` | `TCP_NODELAY` on TLS and HTTP/2 connections, where small records would otherwise stall on Nagle's algorithm combined with delayed acknowledgments. `Enabled` or `Disabled` force it. |
| `zerocopyMode`, `zerocopyMinBytes` | `Opportunistic`, 128 KiB | [`MSG_ZEROCOPY`](#msg_zerocopy) for the response writes of at least `zerocopyMinBytes` of plaintext HTTP/1.1 connections. Below about 10 KiB, pinning pages costs more than copying. |
| `maxZerocopyPendingBytes` | 4 MiB | Response bytes a connection may retain until the completion of their zerocopy sends; beyond it, its writes are copied until completions release some. |
| `maxOutboundBufferBytes` | 4 MiB | Response data queued for a connection that reads slowly. Beyond it, an HTTP/1.1 connection is closed once flushed, and HTTP/2 stops reading frames. |
| `maxCachedConnections` | 10 | Closed connection objects kept for reuse. |
| `minCapturedBodySize` | 1 KiB | Owned response bodies smaller than this are copied next to the head. |

On Linux, a write to a connection reset by the peer fails with `EPIPE` instead of raising `SIGPIPE`, whose default action kills the process: plaintext writes use `MSG_NOSIGNAL`, and event-loop threads block `SIGPIPE` while they run, for the writes that cannot pass a flag (`sendfile()`, OpenSSL). The process-wide `SIGPIPE` disposition is left untouched; threads created from an event-loop thread inherit the blocked signal.

## MSG_ZEROCOPY

On Linux, the response writes of plaintext HTTP/1.1 connections of at least `zerocopyMinBytes` use `MSG_ZEROCOPY`: the kernel sends the bytes from the response buffer instead of copying them into the socket buffer, saving CPU and memory bandwidth on large responses. The listening socket enables it (`SO_ZEROCOPY`, inherited by the accepted sockets) when the kernel supports it (4.14 and later); otherwise, and with `zerocopyMode` `Disabled`, responses are copied as usual.

The kernel reads the bytes after the write returned, until it reports the completion of the send, once the peer acknowledged them. Until then, the server keeps the buffer alive and unchanged:

- Completions arrive in the socket error queue, which the event loop reads when the socket reports `EPOLLERR`, releasing each buffer as soon as its sends completed. A static body ([`bodyStatic()`](../guides/responses.md#bodies)) must stay unchanged until then too.
- A connection retains at most `maxZerocopyPendingBytes` of response bytes (a single larger response may use zerocopy when nothing else is retained): beyond it, its writes are copied until completions release retained buffers.
- A graceful close (`Connection: close`, peer EOF, drain) waits for the completion of the sends. An idle keep-alive connection whose sends are still in flight at `keepAliveTimeout` is closed once they complete, as the kernel would finish sending copied data after a close.
- The sends in flight must progress: a connection whose sends make no progress during a whole `keepAliveTimeout` without any other activity (a client that stopped reading), with or without keep-alive, is reset. So is a connection forcibly closed with sends in flight (server stop, drain deadline, timeouts): the kernel then drops its send queue, and the server keeps its buffers one more second, for the packets already handed to the network device.

`Opportunistic` mode (the default) skips loopback peers, and stops using `MSG_ZEROCOPY` on a connection once the kernel reports that it copied the data anyway: a peer of the same host, through a container network for instance, or a network device without scatter-gather. `Enabled` uses it for every connection, which is only useful to test it locally: the kernel always copies the data sent to a local socket, so measuring the gains needs the client on another host.

`MSG_ZEROCOPY` is not used for TLS (kTLS sockets reject it, and user-space TLS copies the data while encrypting it), HTTP/2, WebSocket and CONNECT tunnels (their output buffers are reused once written), nor for files, which `sendfile()` already sends without copy.

Each zerocopy send pins its pages, which counts against the locked memory limit of the user (`RLIMIT_MEMLOCK`, unless the process has `CAP_IPC_LOCK`) and the option memory of the socket (`net.core.optmem_max`). When they are exhausted, the send fails with `ENOBUFS`: the server logs it once, and copies its responses for one second before trying again. Containers often start with a 64 KiB or 8 MiB locked memory limit: raise it (`docker run --ulimit memlock=...`, `LimitMEMLOCK=` in a systemd unit) for servers sending many large responses at once.

Tests: [zerocopy_test.cpp](../../aeronet/sys/test/zerocopy_test.cpp) (sends, completions out of order, real loopback sends), [connection-state_test.cpp](../../aeronet/http/test/connection-state_test.cpp) (buffer retention), [tests/http-core_test.cpp](../../tests/http-core_test.cpp) (`ZerocopyMode.*`: slow and stalled readers, retention limit, `ENOBUFS`, local peers).

## Writing fast handlers

- Build responses with `req.makeResponse()`, and fill them in order: status, headers, body, trailers (see [Responses](../guides/responses.md)).
- Move large bodies into responses (`body(std::move(buffer))`) instead of copying them, and serve files with `file()` or the [static file handler](../guides/static-files.md).
- Prefer `headerAddLine()` to `header()` when the header cannot be present yet.
- Never block the event loop: move blocking work to `deferWork()` in an [async handler](../guides/async-handlers.md#performance), whose measurements show the cost of each approach.
- Restrict compression to compressible content types (`contentTypeAllowList`), and pick codec levels that fit your CPU budget.

## Measuring

`server.stats()` returns per-server counters; these describe output backpressure and [`MSG_ZEROCOPY`](#msg_zerocopy):

| `ServerStats` field | Meaning |
| --- | --- |
| `totalBytesQueued` | Response bytes accepted for sending. |
| `totalBytesWrittenImmediate`, `totalBytesWrittenFlush` | Bytes written at once, and bytes written later when the socket became writable. |
| `deferredWriteEvents`, `flushCycles` | How often output had to wait for the socket. |
| `maxConnectionOutboundBuffer` | Largest output queue of a connection: compare it with `maxOutboundBufferBytes`. |
| `zerocopySends`, `zerocopyBytes` | `MSG_ZEROCOPY` sends, and the bytes they sent. |
| `zerocopyCopiedSends` | Zerocopy sends whose data the kernel copied anyway (local peer, device without scatter-gather). |
| `zerocopyNoBufferSpaceFallbacks` | Zerocopy sends that failed with `ENOBUFS` (locked or socket option memory exhausted), each followed by one second of copies. |
| `zerocopyRetainLimitFallbacks` | Writes copied because their connection already retained `maxZerocopyPendingBytes`. |
| `zerocopyResetConnections` | Connections reset with zerocopy sends in flight (stalled reader, forced close). |

`MultiHttpServer::stats()` returns the counters of each worker (`per`) and their sum (`total`). The [benchmarks page](../BENCHMARKS.md) describes the benchmark suites, how CI runs them, and how to profile the server.
