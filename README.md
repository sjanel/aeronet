# Aeronet

![Aeronet Logo](resources/logo.png)

HTTP/1.1 server library for Linux only – work in progress.

Features currently implemented:

- Epoll based edge-triggered event loop (one thread per HttpServer)
- Minimal zero-copy-ish HTTP/1.1 request parsing (request line + headers + Content-Length body)
- Chunked Transfer-Encoding (requests) decoding (no trailers exposed yet)
- Basic response builder (status line + headers + body convenience)
- Keep-Alive (with timeout + max requests per connection)
- Pipelined sequential request handling on a single connection
- Configurable limits: max header size, max body size
- Date header caching (1 update / second) to reduce formatting cost
- HEAD method support (suppresses body, preserves Content-Length)
- Expect: 100-continue handling
- Graceful shutdown via runUntil()
- Move semantics for HttpServer (transfer listening socket + loop)
- Multi-reactor horizontal scaling via SO_REUSEPORT
- Optional per-path handler routing with method allow‑lists (exact path match)
- Transparent heterogeneous lookup for path handlers (std::string / std::string_view / const char*)
- Backpressure-aware outbound buffering with statistics for tuning (shared by fixed and streaming responses)
- Multi-instance orchestration wrapper (`MultiHttpServer`) for convenient horizontal scaling (ephemeral port resolution + aggregated stats)
- Lightweight spdlog-style logging API with ISO 8601 UTC timestamps (fallback internally; pluggable interface planned)
- Tests for basics, move semantics, reuseport distribution, keep-alive, header/body limits, chunked, HEAD, Expect, multi-server wrapper

## HTTP/1.1 Feature Matrix

Legend: [x] implemented, [ ] planned / not yet.

Core parsing & connection handling

- [x] Request line parsing (method, target, version)
- [x] Header field parsing (no folding / continuations)
- [x] Case-insensitive header lookup helper
- [x] Persistent connections (HTTP/1.1 default, HTTP/1.0 opt-in)
- [x] HTTP/1.0 response version preserved (no silent upgrade)
- [x] Connection: close handling
- [x] Pipelined sequential requests (no parallel handler execution)
- [x] Backpressure / partial write buffering

Request bodies

- [x] Content-Length bodies with size limit
- [x] Chunked Transfer-Encoding decoding (request) (ignores trailers)
- [ ] Trailer header exposure
- [ ] Multipart/form-data convenience utilities

Response generation

- [x] Basic fixed body responses
- [x] HEAD method (suppressed body, correct Content-Length)
- [x] Outgoing chunked / streaming responses (basic API: status/headers + incremental write + end, keep-alive capable)
- [ ] Compression (gzip / br)

Status & error handling

- [x] 400 Bad Request (parse errors, CL+TE conflict)
- [x] 413 Payload Too Large (body limit)
- [x] 431 Request Header Fields Too Large (header limit)
- [x] 501 Not Implemented (unsupported Transfer-Encoding)
- [x] 505 HTTP Version Not Supported
- [x] 400 on HTTP/1.0 requests carrying Transfer-Encoding
- [ ] 415 Unsupported Media Type (content-type based) – not required yet
- [x] 405 Method Not Allowed (enforced when path exists but method not in allow set)

Headers & protocol niceties

- [x] Date header (cached once per second)
- [x] Connection keep-alive / close
- [x] Content-Type (user supplied only)
- [x] Expect: 100-continue handling
- [x] Expect header ignored for HTTP/1.0 (no interim 100 sent)
- [ ] Server header (intentionally omitted to keep minimal)
- [ ] Access-Control-* (CORS) helpers

Performance / architecture

- [x] Single-thread event loop (one server instance)
- [x] Horizontal scaling via SO_REUSEPORT (multi-reactor)
- [x] Multi-instance orchestration wrapper (`MultiHttpServer`) (auto reuseport, aggregated stats)
- [x] writev scatter-gather for response header + body
- [x] Outbound write buffering with EPOLLOUT-driven backpressure
- [ ] Benchmarks & profiling docs
- [ ] Zero-copy sendfile() support for static files

Safety / robustness

- [x] Configurable header/body limits
- [x] Graceful shutdown loop (runUntil)
- [ ] Slowloris style header timeout mitigation (per-connection read deadline)
- [ ] TLS termination (OpenSSL) – currently only linked, not enabled

Developer experience

- [x] Builder style ServerConfig
- [x] Simple lambda handler signature
- [x] Simple exact-match per-path routing (`addPathHandler`)
- [x] Lightweight built-in logging (spdlog optional integration) – pluggable interface TBD
- [ ] Middleware helpers
- [ ] Pluggable logging interface (abstract sink / formatting hooks)

Misc

- [x] Move semantics for HttpServer
- [x] MultiHttpServer convenience wrapper
- [ ] Compression (gzip / br) (planned)
- [ ] Public API stability guarantee (pre-1.0)
- [ ] License file

## Test Coverage Matrix

Summary of current automated test coverage (see `tests/` directory). Legend: ✅ covered by explicit test(s), ⚠ partial / indirect, ❌ not yet.

| Area | Feature | Test Status | Notes / Test Files |
|------|---------|-------------|--------------------|
| Parsing | Request line (method/target/version) | ✅ | `http_basic.cpp`, malformed cases in `http_malformed.cpp` |
| Parsing | Unsupported HTTP version (505) | ✅ | `http_parser_errors.cpp` (InvalidVersion505) |
| Parsing | Header parsing & lookup | ✅ | Implicit in all request tests; malformed headers in `http_malformed.cpp` |
| Limits | Max header size -> 431 | ✅ | `http_malformed.cpp` (OversizedHeaders) |
| Limits | Max body size (Content-Length) -> 413 | ✅ | `http_additional.cpp` (ExplicitTooLarge413) |
| Limits | Chunk total/body growth -> 413 | ✅ | Chunk oversize paths indirectly via size guard (add targeted test later if needed) |
| Bodies | Content-Length body handling | ✅ | `http_basic.cpp`, others |
| Bodies | Chunked decoding | ✅ | `http_chunked_head.cpp`, fuzz in `http_parser_errors.cpp` |
| Bodies | Trailers exposure | ❌ | Not implemented |
| Expect | 100-continue w/ non-zero length | ✅ | `http_parser_errors.cpp` (Expect100OnlyWithBody) |
| Expect | No 100 for zero-length | ✅ | `http_parser_errors.cpp` (Expect100OnlyWithBody) & `http_additional.cpp` |
| Keep-Alive | Basic keep-alive persistence | ✅ | `http_keepalive.cpp` |
| Keep-Alive | Max requests per connection | ✅ | `http_additional.cpp` (CloseAfterLimit), `http_head_maxrequests.cpp` |
| Keep-Alive | Idle timeout close | ⚠ | Covered indirectly (tests rely on timely closure); no explicit timeout assertion |
| Pipelining | Sequential pipeline of requests | ✅ | `http_additional.cpp` (TwoRequestsBackToBack) |
| Pipelining | Malformed second request handling | ✅ | `http_additional.cpp` (SecondMalformedAfterSuccess) |
| Methods | HEAD semantics (no body) | ✅ | `http_chunked_head.cpp`, `http_head_maxrequests.cpp` |
| Date | RFC7231 format + correctness | ✅ | `http_date.cpp` (format + caching tests) |
| Date | Same-second caching invariance | ✅ | `http_date.cpp` (StableWithinSameSecond) |
| Date | Second-boundary refresh | ✅ | `http_date.cpp` (ChangesAcrossSecondBoundary) |
| Errors | 400 Bad Request (malformed line) | ✅ | `http_malformed.cpp` |
| Errors | 431, 413, 505, 501 | ✅ | Various tests (`http_malformed.cpp`, `http_additional.cpp`, version & TE tests) |
| Errors | PayloadTooLarge in chunk decoding | ⚠ | Path exercised via guard; add dedicated oversize chunk test future |
| Concurrency | SO_REUSEPORT distribution | ✅ | `http_multi_reuseport.cpp` |
| Lifecycle | Move semantics of server | ✅ | `http_server_move.cpp` |
| Lifecycle | Graceful stop (runUntil) | ✅ | All tests using runUntil pattern |
| Diagnostics | Parser error callback (version, bad line, limits) | ✅ | `http_parser_errors.cpp` |
| Diagnostics | PayloadTooLarge callback (Content-Length) | ⚠ | Indirect; could add explicit capture test |
| Performance | Date caching buffer size correctness | ✅ | Indirect via date tests + no crash (unit test ensures length) |
| Performance | writev header+body path | ⚠ | Indirect (no specific assertion) |
| Not Implemented | Trailers, outgoing chunked, compression, routing, TLS | ❌ | Roadmap |

Planned test additions (nice-to-have): oversize single chunk explicit test, keep-alive idle timeout explicit assertion, payload-too-large callback capture for chunk paths, writev behavior inspection (mock / interception).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Construction Model (RAII) & Ephemeral Ports

`HttpServer` binds, sets socket options, enters listening state, and registers the listening fd with epoll inside its constructor (RAII). If you request an ephemeral port (`port = 0` in `ServerConfig`), the kernel-assigned port is immediately available via `server.port()` after construction (no separate `setupListener()` call required – that legacy function was removed during refactor).

Why RAII?

- Guarantees a fully initialized, listening server object or a thrown exception (no half-initialized state)
- Simplifies lifecycle (no forgotten setup step)
- Enables immediate test usage with ephemeral ports

Ephemeral port pattern in tests / examples:

```cpp
ServerConfig cfg; // let kernel choose the port
HttpServer server(cfg);
uint16_t actual = server.port(); // resolved port
```

NOTE: A previous experimental non-throwing `tryCreate` factory was removed to reduce API surface; the throwing constructor is the only creation path for now.

## Quick Usage Examples

### Minimal Global Handler (Ephemeral Port)

```cpp
#include <aeronet/server.hpp>
#include <aeronet/server-config.hpp>
using namespace aeronet;

int main() {
  HttpServer server(cfg.withPort(8080));
  std::printf("Listening on %u\n", server.port());
  server.setHandler([](const HttpRequest& req) {
    HttpResponse r;
    r.statusCode = 200; r.reason = "OK";
    r.body = "Hello from Aeronet\n";
    r.contentType = "text/plain";
    return r;
  });
  server.runUntil([]{ return false; }); // press Ctrl+C to terminate process
}
```

### Per-Path Routing & Method Masks

```cpp
HttpServer server(ServerConfig{}); // ephemeral port

server.addPathHandler("/hello", http::MethodSet{http::Method::GET}, [](const HttpRequest&){
  HttpResponse r; r.statusCode=200; r.reason="OK"; r.contentType="text/plain"; r.body="world"; return r; });

// Add POST later (merges methods)
server.addPathHandler("/hello", http::Method::POST, [](const HttpRequest& req){
  HttpResponse r; r.statusCode=200; r.reason="OK"; r.contentType="text/plain"; r.body=req.body; return r; });

// Unknown path -> 404, known path wrong method -> 405 automatically.

server.runUntil([]{ return false; });
```

### Multi‑Reactor (SO_REUSEPORT) Launch Sketch

```cpp
std::vector<std::thread> threads;
for (int i = 0; i < 4; ++i) {
  threads.emplace_back([i]{
  ServerConfig cfg; cfg.withPort(8080).withReusePort(true); // or 0 for ephemeral resolved separately
    HttpServer s(cfg);
    s.setHandler([](const HttpRequest&){ HttpResponse r{200, "OK"}; r.body="hi"; r.contentType="text/plain"; return r; });
    s.runUntil([]{ return false; });
  });
}
for (auto& t : threads) t.join();
```

### Accessing Backpressure / IO Stats

```cpp
auto st = server.stats();
std::printf("queued=%llu imm=%llu flush=%llu defer=%llu cycles=%llu maxConnBuf=%zu\n",
  (unsigned long long)st.totalBytesQueued,
  (unsigned long long)st.totalBytesWrittenImmediate,
  (unsigned long long)st.totalBytesWrittenFlush,
  (unsigned long long)st.deferredWriteEvents,
  (unsigned long long)st.flushCycles,
  st.maxConnectionOutboundBuffer);
```

## Run example

```bash
./build/examples/aeronet-minimal 8080
```

Then visit <http://localhost:8080/>

### Multi-reactor (SO_REUSEPORT) example

You can start several independent event loops on the same port (kernel load balances incoming connections) by using
`enablePortReuse(true)` and running one `HttpServer` per thread:

```bash
./build/examples/aeronet-multi 8080 4   # port 8080, 4 threads
```

Each thread owns its own listening socket (SO_REUSEPORT) and epoll instance – no shared locks in the accept path.
This is the simplest horizontal scaling strategy before introducing a worker pool.

### MultiHttpServer Convenience Wrapper

Instead of manually creating N threads and N `HttpServer` instances, you can use `MultiHttpServer` to spin up a "farm" of identical servers on the same port. It:

- Accepts a base `ServerConfig` (set `port=0` for ephemeral bind; the chosen port is propagated to all instances)
- Forces `reusePort=true` automatically when thread count > 1
- Replicates either a global handler or all registered path handlers across each underlying server
- Exposes `stats()` returning both per-instance and aggregated totals (sums; `maxConnectionOutboundBuffer` is a max)
- Manages lifecycle with internal `std::jthread`s; `stop()` requests shutdown of every instance
- Provides the resolved listening `port()` after start (even for ephemeral port 0 requests)

Minimal example:

```cpp
#include <aeronet/multi-http-server.hpp>
using namespace aeronet;

int main() {
  ServerConfig cfg; cfg.port = 0; cfg.reusePort = true; // ephemeral, auto-propagated
  MultiHttpServer multi(cfg, 4); // 4 underlying event loops
  multi.setHandler([](const HttpRequest& req){
    HttpResponse r; r.statusCode=200; r.reason="OK"; r.body="hello\n"; r.contentType="text/plain"; return r; });
  multi.start();
  std::printf("Listening on %u\n", multi.port());
  // ... run until external signal ...
  std::this_thread::sleep_for(std::chrono::seconds(30));
  auto agg = multi.stats();
  std::printf("instances=%zu queued=%llu\n", agg.per.size(), (unsigned long long)agg.total.totalBytesQueued);
  multi.stop();
}
```

Handler rules mirror `HttpServer`:

- Call `setHandler()` once OR register multiple `addPathHandler()` entries before `start()`.
- Mixing global handler with path handlers is rejected.
- After `start()`, further registration throws.

Ephemeral port binding: `multi.start()` launches threads and spin‑waits briefly until the first server resolves its kernel-assigned port (captured via `getsockname()` in the internal `HttpServer`). The resolved value is then visible via `multi.port()` and reused for all subsequent instances.

Stats aggregation example:

```cpp
auto st = multi.stats();
for (size_t i = 0; i < st.per.size(); ++i) {
  const auto& s = st.per[i];
  std::printf("[srv%zu] queued=%llu imm=%llu flush=%llu\n", i,
    (unsigned long long)s.totalBytesQueued,
    (unsigned long long)s.totalBytesWrittenImmediate,
    (unsigned long long)s.totalBytesWrittenFlush);
}
```

### Logging

Logging uses `spdlog` if `AERONET_ENABLE_SPDLOG` is defined at build time; otherwise a lightweight fallback provides the same call style (`log::info("message {}", value)`). The fallback uses `std::vformat` when available and degrades gracefully if formatting fails (appends arguments). Timestamps are ISO 8601 UTC with millisecond precision. Levels: trace, debug, info, warn, error, critical. You can adjust level in fallback with `aeronet::log::set_level(aeronet::log::level::debug);`.

Pluggable logging sinks / structured logging hooks are planned; current design keeps logging dependency-free by default.

## Streaming Responses (Chunked / Incremental)

The streaming API lets a handler produce a response body incrementally without a priori knowing its full size.
Register a streaming handler instead of the fixed response/global or per-path handlers:

```cpp
HttpServer server(ServerConfig{}.withPort(8080));
server.setStreamingHandler([](const HttpRequest& req, HttpResponseWriter& w) {
  w.setStatus(200, "OK");
  w.setHeader("Content-Type", "text/plain");
  // Optionally set Content-Length first if known to disable chunked encoding:
  // w.setContentLength(totalBytes);
  for (int i = 0; i < 5; ++i) {
    if (!w.write("chunk-" + std::to_string(i) + "\n")) {
      // Backpressure or failure: in this phase writer returns false if connection marked to close.
      break;
    }
  }
  w.end();
});
```

Key semantics:

- By default responses are sent with `Transfer-Encoding: chunked` unless `setContentLength()` is called before any body bytes are written.
- `write()` queues data into the server's outbound buffer (no direct syscalls in the writer) and returns `false` if the connection was marked for closure (e.g., max outbound buffer exceeded or hard error). In future phases it may return `false` transiently to signal backpressure without connection closure.
- `end()` finalizes the response. For chunked mode it appends the terminating `0\r\n\r\n` chunk.
- HEAD requests automatically suppress body bytes; you can still call `write()` for symmetric code paths.
- Keep-Alive is supported for streaming responses when: server keep-alive is enabled, HTTP/1.1 is used, max-requests-per-connection not exceeded, and the connection wasn't marked for closure due to buffer overflow or error. Set an explicit `Connection: keep-alive` header inside your streaming handler if you want to guarantee the header presence; otherwise the server may add it according to policy (future enhancement).
- If you provide your own `Connection: close` header, the server will honor it, preventing reuse.

Backpressure & buffering:

- Streaming writes reuse the same outbound buffering subsystem as fixed responses (`queueData`). Immediate socket writes are attempted when the buffer is empty; partial or blocked writes append to the per-connection buffer and register EPOLLOUT interest. This unification reduced code paths and eliminated a previous duplication in streaming flush logic.
- If the per-connection buffered bytes exceed `maxOutboundBufferBytes`, the connection is marked to close after flush, and subsequent `write()` calls return `false`.

Limitations / roadmap (streaming phase 1):

- No trailer support yet.
- Backpressure signaling currently binary (accept / fatal) – future versions may expose a tri-state (ok / should-pause / failed).
- Compression not yet integrated with streaming.

Testing:

- See `tests/http_streaming.cpp` (basic chunk framing & HEAD) and `tests/http_streaming_keepalive.cpp` (keep-alive reuse) for examples.


## Configuration API (builder style)

`ServerConfig` lives in `aeronet/server-config.hpp` and exposes fluent setters (withX naming):

```cpp
ServerConfig cfg;
cfg.withPort(8080)
  .withReusePort(true)
  .withMaxHeaderBytes(16 * 1024)
  .withMaxBodyBytes(2 * 1024 * 1024)
  .withKeepAliveTimeout(std::chrono::milliseconds{10'000})
  .withMaxRequestsPerConnection(500)
  .withKeepAliveMode(true);

HttpServer server(cfg); // or HttpServer(8080) then server.setConfig(cfgWithoutPort);
```

Keep-alive can be disabled globally by `cfg.withKeepAliveMode(false)`; per-request `Connection: close` or `Connection: keep-alive` headers are also honored (HTTP/1.1 default keep-alive, HTTP/1.0 requires explicit header).

### Handler Registration / Routing (Detailed)

Two mutually exclusive approaches:

1. Global handler: `server.setHandler([](const HttpRequest&){ ... })` (receives every request).
2. Per-path handlers: `server.addPathHandler("/hello", http::MethodsSet{http::Method::GET, http::Method::POST}, handler)` – exact path match.

Rules:

- Mixing the two modes (calling `addPathHandler` after `setHandler` or vice-versa) throws.
- If a path is not registered -> 404 Not Found.
- If path exists but method not allowed -> 405 Method Not Allowed.
- Allowed methods are supplied as a non-allocating `http::MethodSet` (small fixed-capacity container) containing `http::Method` values.
- You can call `addPathHandler` repeatedly on the same path to extend the allowed method mask (handler is replaced, methods merged).

Example:

```cpp
HttpServer server(cfg);
server.addPathHandler("/hello", http::MethodsSet{http::Method::GET}, [](const HttpRequest&){
  HttpResponse r; r.statusCode=200; r.reason="OK"; r.body="world"; r.contentType="text/plain"; return r; });
server.addPathHandler("/echo", http::MethodsSet{http::Method::POST}, [](const HttpRequest& req){
  HttpResponse r; r.statusCode=200; r.reason="OK"; r.body=req.body; r.contentType="text/plain"; return r; });
// Add another method later (merges method mask, replaces handler)
server.addPathHandler("/echo", http::Method::GET, [](const HttpRequest& req){
  HttpResponse r; r.statusCode=200; r.reason="OK"; r.body = "Echo via GET"; r.contentType="text/plain"; return r; });
```

Internal bitmask order follows enum declaration in `http-method.hpp`.

### Limits

- 431 is returned if the header section exceeds `maxHeaderBytes`.
- 413 is returned if the declared `Content-Length` exceeds `maxBodyBytes`.
- Connections exceeding `maxOutboundBufferBytes` (buffered pending write bytes) are marked to close after flush (default 4MB) to prevent unbounded memory growth if peers stop reading.

### Performance / Metrics & Backpressure

`HttpServer::stats()` exposes aggregated counters:

- `totalBytesQueued` – bytes accepted into outbound buffering (including those sent immediately)
- `totalBytesWrittenImmediate` – bytes written synchronously on first attempt (no buffering)
- `totalBytesWrittenFlush` – bytes written during later flush cycles (EPOLLOUT)
- `deferredWriteEvents` – number of times EPOLLOUT was registered due to pending data
- `flushCycles` – number of flush attempts triggered by writable events
- `maxConnectionOutboundBuffer` – high-water mark of any single connection's buffered bytes

Use these to gauge backpressure behavior and tune `maxOutboundBufferBytes`. When a connection's pending buffer would exceed the configured maximum, it is marked for closure once existing data flushes, preventing unbounded memory growth under slow-reader scenarios.

### Roadmap additions

- [x] Connection write buffering / partial write handling
- [x] Outgoing chunked responses & streaming interface (phase 1)
- [ ] Trailing headers exposure for chunked requests
- [ ] Richer routing (wildcards, parameter extraction)
- [ ] TLS (OpenSSL) support
- [ ] Benchmarks & perf tuning notes

## License

Licensed under the MIT License. See [LICENSE](LICENSE).
