# aeronet Roadmap — Planned / Not Implemented

## High priority

- **HTTP/2 Performance Optimization & Testing** (h2load benchmarks):
  - Micro-benchmarks for stream multiplexing efficiency
  - h2load-based load testing scenarios (concurrent streams, various payload sizes)
  - Performance regression detection in CI
  - Identify and optimize hot paths in HPACK, flow control, and frame processing
- **Security Hardening Audit**:
  - TLS fingerprinting hardening (avoid leaking version/cipher info in edge cases)
  - Memory scrubbing for sensitive data (handshake keys, session tickets)
  - Fuzzing harness integration (libFuzzer + AFL)

## Medium priority

- **Windows event loop performance**: The Windows backend uses WSAPoll (readiness‑based, like epoll/kqueue) which is functionally correct but less performant than IOCP for high‑concurrency workloads. A future IOCP backend would require a fundamental architecture shift from readiness to completion semantics.
- **macOS `EVFILT_TIMER` integration**: `TimerFd::armPeriodic()` on macOS is currently a no-op and relies on poll timeouts. Using kqueue's native `EVFILT_TIMER` would improve timer precision but requires event-loop refactoring to accommodate heterogeneous kqueue filter types.
- **Structured logging / pluggable sinks** - Basic logging functional; spdlog backend supports custom sinks/formatters; an aeronet-native sink registration API is not yet exposed
- **Enhanced parser diagnostics** (byte offset in parse errors for better debugging)
- **Direct compression option for HEAD**: optional config to allow HEAD responses to match GET headers
  (Content-Encoding + compressed Content-Length) when desired.
- **Performance improvements**:
  - ~~`TCP_CORK` / `TCP_NOPUSH` for response header/body coalescing~~ ✔ (Linux: `TcpCorkGuard` RAII in `queueData` / `flushOutbound`; macOS: no-op because `TCP_NOPUSH` does not flush on clear — `writev` already coalesces)
  - Further hot-path cache locality optimization
- Enhance `telemetry` with more detailed HTTP/2 metrics: per-stream stats, HPACK compression ratios, frame type distributions.
  - Support tags/labels for metrics

### Performance improvement ideas

#### Completed

- ~~`TCP_CORK` / `TCP_NOPUSH` to coalesce~~ ✔

#### High priority

- **HPACK dynamic table: replace `vector` with `deque`** — `_entries.insert(_entries.begin(), ...)` in `hpack.cpp` is O(n) per new header insertion (shifts all elements). `std::deque` gives O(1) `push_front()`, O(1) `pop_back()` eviction, and preserves random-access indexing. Expected 10–20 % improvement on header-heavy HTTP/2 workloads cycling through the dynamic table.
- **HPACK static table: hash-based encoding lookup** — the encoding path linearly scans the 61-entry static table to match header names. Build a compile-time `flat_hash_map` or `constexpr` indexed array for O(1) lookup. Expected 5–15 % gain on typical HTTP/2 traffic with standard headers (`:method`, `:path`, `content-type`, …).
- **HTTP/2 stream cleanup: consolidate per-stream maps** — stream close triggers 5 separate map erasures (`_streamRequests`, `_pendingFileSends`, `_pendingStreamingSends`, `_pendingAsyncTasks`, tunnel maps) in `Http2ProtocolHandler::onStreamClosed()`. Unify per-stream state into a single struct stored in one map (optional fields via variant or flags for stream mode). Reduces hash lookups from 5 → 1 per stream close, better cache locality for stream lifecycle.
- ~~**Response writer: pool compression output buffer**~~ ✅ Done — `_compressedBuffer` member replaces per-call `RawChars` allocations in `writeBody()`, `accumulateInPreCompressBuffer()`, and `end()`. Buffer capacity only grows, never freed between chunks. Tests: `HttpStreamingCompression.MultiChunkCompressedWriteReusesBuffer`.

#### Medium priority

- **`ConnectionState` bool bit-packing** — 10 scattered `bool` fields + `CloseMode` / `ProtocolType` enums waste bytes to padding between larger fields. Pack booleans into a `uint16_t` bitfield, or reorder all fields by descending size. Saves ~16–32 bytes per `ConnectionState` — meaningful at 10 K+ connections.
- **Brotli encoder context reuse across sessions** — Brotli state is destroyed and recreated each session, unlike Zstd (`ZSTD_CCtx_reset()`) and Zlib (stream reuse). Explore caching the `BrotliEncoderState` and resetting parameters between sessions, or a lighter reinit pattern.
- **Adaptive event loop poll timeout** — currently fixed at construction time. If last `poll()` returned max events, use zero timeout (spin); if idle for N iterations, increase timeout exponentially up to a cap. Better tail latency under bursts, lower CPU when idle.
- **Keep-alive timeout: timer wheel instead of sweep** — idle connection cleanup in `ConnectionManager` sweeps all connections (O(n)). A timer wheel or min-heap sorted by expiry lets each poll iteration check only connections whose expiry has passed → O(expired). Significant at 10 K+ idle connections.
- Enforce backpressure correctness to avoid overload and wasted work.
- Focus on cache locality in hot paths; measure before/after.

#### Low priority / specialized

- **WebSocket frame demasking: full SIMD XOR** — SSE4.2 is used for mask detection, but the actual XOR demasking loop is scalar. Vectorize the 4-byte-repeating XOR with SSE2/AVX2 to process 16–32 bytes per iteration → 2–4× throughput for large binary WebSocket frames.
- **Pre-computed static file response headers** — response headers (`Content-Type`, `Content-Length`, `ETag`, `Last-Modified`) are formatted per request for the same file. Cache fully‑formed header bytes alongside file metadata; invalidate on stat change.
- `io_uring` support for Linux (future major feature, likely separate transport layer implementation).

#### Benchmark gaps

The following micro-benchmarks are missing and should be added to validate the above improvements:

| Gap | Validates |
|-----|-----------|
| HPACK dynamic table churn (insert / evict profile) | Dynamic table `vector` → `deque` & static table hash |
| 1 000+ concurrent stream create / destroy | Per-stream map consolidation |
| Compressed response throughput (per-chunk alloc overhead) | Response writer buffer pooling |
| `ConnectionState` `sizeof` / cache-line analysis | Bool bit-packing |
| WebSocket large-frame demasking throughput | SIMD XOR demasking |
| Connection sweep latency at 10 K idle connections | Timer wheel vs sweep |

## Long-term / Nice-to-have

- **HTTP/3 / QUIC Support** - Likely separate transport layer implementation (future major feature)
- **Fuzz Harness Integration** - libFuzzer targets for HTTP/1.1 and HTTP/2 parsing
- **OCSP Stapling & Advanced TLS** - Passive stapling with cached responses, CRL hooks, key logging (debug only)
- **Per-SNI mTLS Policies** - Different client cert requirements per SNI hostname
- **Advanced Metrics** - ~~Histogram/percentile latency buckets~~ ✔ (implemented via `TelemetryContext::histogram()` + `TelemetryConfig::addHistogramBuckets()`); per-route stats not yet implemented

### Optional feature modules (compile-time gated)

The following features are inspired by capabilities that make frameworks like **Drogon**, **Crow**, and **Beast** popular for building full applications. Each is designed to be **opt-in via a compile-time flag** (`OFF` by default) so they add zero overhead when unused, consistent with aeronet's modular philosophy.

#### HTTP Client (`AERONET_ENABLE_HTTP_CLIENT`)

Async HTTP/1.1 + HTTP/2 client reusing the existing event loop, TLS stack, and compression codecs. Connection pooling with keep-alive, automatic retry, and redirect following. Coroutine-friendly API:

```cpp
auto resp = co_await client.get("https://api.example.com/data");
```

Essential for microservice architectures (proxying, upstream calls, service-to-service health checks). This is Drogon's most-used feature beyond basic serving and a common request for any HTTP server library aiming at real-world deployments.

#### Server-Sent Events (SSE) convenience layer

Thin abstraction over existing chunked streaming — no new compile flag needed. Auto-formats `text/event-stream` content type with `id:`, `event:`, `data:` field framing and `retry:` reconnect hints. API sugar on `HttpResponseWriter`:

```cpp
router.setPath(http::Method::GET, "/events", [](const HttpRequest&, HttpResponseWriter& w) {
  SseWriter sse(w);
  sse.event("update", R"({"temp": 22.5})");
  sse.event("update", R"({"temp": 23.1})", "evt-002"); // with id
});
```

Growing in popularity as a lighter alternative to WebSocket for server → client push (dashboards, notifications, live feeds).

#### Rate Limiting middleware (`AERONET_ENABLE_RATE_LIMIT`)

Built-in token bucket or sliding window rate limiter. Per-IP and/or per-route. Returns `429 Too Many Requests` with `Retry-After` header. Configurable burst / sustained rates. Pluggable backend interface (in-memory default, extensible to Redis or shared stores). Currently only TLS handshake rate limiting exists (`TLSConfig::handshakeRateLimitPerSecond`); this would extend to HTTP request level.

#### Cookie helpers & Session store (`AERONET_ENABLE_SESSIONS`)

- **Cookie builder**: RFC 6265-compliant `Set-Cookie` helper with `SameSite`, `Secure`, `HttpOnly`, `MaxAge`, `Path`, `Domain` attributes. Currently only raw header parsing exists.
- **Session store**: In-memory session store with configurable TTL and pluggable backend interface. HMAC-signed session cookie for integrity. Both Drogon and Crow include this built-in; it is the most basic state management primitive for web applications.

#### JSON request / response convenience (extends `AERONET_ENABLE_GLAZE`)

Extend the existing glaze integration with ergonomic helpers:

- Automatic JSON body parsing with validation error → 400
- `HttpResponse::json(T)` serialization one-liner
- `Content-Type: application/json` auto-detection on request and auto-setting on response
- No new compile flag — enhances existing `AERONET_ENABLE_GLAZE` support

#### Response caching middleware (`AERONET_ENABLE_RESPONSE_CACHE`)

In-memory LRU cache keyed by method + path + `Vary` headers. Respects `Cache-Control` directives (`max-age`, `no-store`, `no-cache`, `private`). Configurable max entries and memory budget. Per-route opt-in via middleware registration. ETag / `If-None-Match` validation for cached entries. Useful for APIs with expensive computation behind cacheable endpoints.

#### Regex route constraints

Optional regex validation on path parameters, extending the existing radix tree router:

```cpp
router.setPath(http::Method::GET, "/users/{id:[0-9]+}", handler);       // digits only
router.setPath(http::Method::GET, "/files/{path:[a-zA-Z0-9/._-]+}", handler); // safe path chars
```

Returns 404 on constraint mismatch. Consider [CTRE](https://github.com/hanickadot/compile-time-regular-expressions) for compile-time regex performance, with `std::regex` fallback. Drogon supports regex routing natively.

### TLS enhancements (detailed roadmap)

#### Phase 3 (Advanced / Enterprise)

- OCSP stapling (passive, cached)
- Optional CRL / revocation hooks
- ~~Histogram / percentile metrics~~ ✔
- Key log (debug only)
- Security hardening audits (zeroization, memory scrub confirmations)

#### Phase 4 (Future Protocol / Extensibility)

- ~~ALPN "h2" groundwork~~ ✔ (HTTP/2 implemented)
- Per-SNI mTLS policies
- Session ticket key rotation scheduling & multi-key window
- (Stretch) Exploring QUIC/HTTP/3 (would likely be a separate transport layer, so only mention if strategic)

## Realistic Network Testing

Goals

- Improve confidence that aeronet behaves correctly under latency, jitter, packet loss, reordering, partial delivery, and connection resets.
- Catch protocol-level bugs (HTTP/1.1 and HTTP/2), TLS handshake fragility, flow-control and resource-leak issues that do not appear on a perfect local loopback.

Approach and Components

- Deterministic simulated-network unit tests (high priority):
  - Add an injectable transport/socket abstraction used by protocol layers so tests can replace the real socket with a `TestSocket` implementation.
    - `TestSocket` capabilities: partial reads/writes, configurable delays (simulated timers), reordering, duplication, injected resets, and deterministic pseudo-randomness with a seed.
    - Target areas: HTTP/2 frame reassembly and flow-control, HTTP/1.1 chunked transfer edge cases, TLS handshake fragmentation handling, and higher-level timeouts.
    - Tests are deterministic, fast, and run in PRs.

- Proxy-based user-space fault injection (medium priority):
  - Integrate Toxiproxy or a small custom proxy harness for tests that exercise full binaries without requiring NET_ADMIN privileges.
  - Use the proxy to inject latency, connection cuts, truncation and partial writes to validate end-to-end behaviors.

- Kernel-level integration using `tc netem` and network namespaces (lower priority):
  - Create e2e tests that run client & server in separate Linux network namespaces connected by a veth pair with `tc netem` rules applied.
  - Simulate real TCP behaviours (retransmits, delayed ACKs, segment coalescing, zero-window events) that only the kernel stack exhibits.
  - These tests are heavier and run in nightly or scheduled CI only; they are optional for PRs because they require privileged runners and can be flaky.

Test Design & Best Practices

- Start with syscall error injection tests (EINTR, EAGAIN, EPIPE, ECONNRESET) and partial I/O.
- Prefer deterministic `TestSocket` unit tests for core protocol logic: easier to reproduce and debug.
- For integration tests, capture detailed artifacts on failures: pcap, logs, and deterministic seeds used by the test harness.
- Expose test-time configuration hooks (shorter timeouts, deterministic timers) so tests run quickly and reliably.

CI Policy

- PRs: run all deterministic unit tests (including `TestSocket` simulated-network tests).
- Nightly: run proxy-based and `tc netem` integration suites; mark these jobs non-blocking for PRs.

Milestones (suggested)

1. Add `TestSocket` abstraction and 30 deterministic unit tests covering HTTP/2 framing, window-update races, and HTTP/1.1 chunked edge cases.
2. Add simple Toxiproxy-based harness and a handful of end-to-end tests (connection cut, high latency, truncated responses).
3. Add `ip netns` + `tc netem` scripts in `tests/e2e/` and integrate nightly CI job.
4. Collect failure artifacts (pcap + logs) and add tooling to reproduce failing scenarios locally with the same netem parameters.

Acceptance Criteria

- Protocol correctness under simulated faults: no state corruption, proper error propagation, and graceful teardown.
- No resource leaks (sockets, memory) in faulted runs.
- Deterministic unit tests reproduce failures locally with a seed and do not rely on privileged resources.

Notes

- Adding a transport abstraction is a small API design change but yields large testability benefits. Keep the abstraction minimal and efficient in production builds (thin indirection).
- Proxy-based tests are useful when privileged operations are not available in CI.
