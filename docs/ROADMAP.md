# aeronet Roadmap - Planned / Not Implemented

## High priority

- **Security Hardening Audit**:
  - TLS fingerprinting hardening (avoid leaking version/cipher info in edge cases)
  - Memory scrubbing for sensitive data (handshake keys, session tickets)
  - Fuzzing harness integration (libFuzzer + AFL)
- Create doc pages using **Material for MkDocs** tool (for instance). To create first class documentation for aeronet, we need to have a proper documentation site with a good theme and navigation. This will help users understand how to use the library and its features.
- Make a real `HttpRequest` object. `HttpRequestView` and `HttpResponse` share a lot of semantics, except for the status-line. We could extract common bricks in a base `HttpMessage` class and have HttpResponse and HttpRequest derive from it (unfortunately, HttpRequestView is the name of the class coming from a view on a request arriving in a server, so we cannot rename it). This will allow us to have a proper HttpRequest object that can be used to send requests to other servers.
- Rename `HttpRequestView` to `HttpRequestView` and keep `HttpRequestView` as a builder object for the HTTP client (this will help to have a clearer naming especially for above requirement).

## Medium priority

- **Windows event loop performance**: The Windows backend uses WSAPoll (readiness‑based, like epoll/kqueue) which is functionally correct but less performant than IOCP for high‑concurrency workloads. A future IOCP backend would require a fundamental architecture shift from readiness to completion semantics.
- **macOS `EVFILT_TIMER` integration**: `TimerFd::armPeriodic()` on macOS is currently a no-op and relies on poll timeouts. Using kqueue's native `EVFILT_TIMER` would improve timer precision but requires event-loop refactoring to accommodate heterogeneous kqueue filter types.
- **Pluggable logging sink API (non-access logs)** - spdlog backend supports custom sinks/formatters; an aeronet-native sink registration API is not yet exposed.
- **Enhanced parser diagnostics** (byte offset in parse errors for better debugging)
- **Direct compression option for HEAD**: optional config to allow HEAD responses to match GET headers
  (Content-Encoding + compressed Content-Length) when desired.
- Enhance `telemetry` with more detailed HTTP/2 metrics: per-stream stats, HPACK compression ratios, frame type distributions.
  - Support tags/labels for metrics
- **Manage TE header in the client**: currently the TE header is reserved in the framework, so it cannot be sent. It could be a good idea to have a flag in the config to allow sending TE headers in the client with trailers essentially (because trailers are natively supported by aeronet). If `TE: trailers` is sent, in HTTP/1.1 we also need to add `Connection: TE`.

### Performance optimization backlog

Status: repository-wide source audit completed on 2026-07-22; the opportunities below are not yet implemented or
proven faster. They are ordered by expected leverage, not by unverified percentage estimates.

#### P1 - copies, allocations and asymptotic hot paths

- **HTTP/2 outbound frame representation** - `Http2Connection::sendData()` currently copies every payload behind a
  9-byte frame header in `_outputBuffer`, and `encodeHeaders()` moves then recopies oversized HPACK blocks when emitting
  CONTINUATION frames. Design a queued-fragment / gather-write representation that can retain payload ownership across
  partial writes and flow-control stalls, then use scatter I/O where the transport supports it. This shared HTTP/2 core
  serves both client and server, so validate response streaming, client uploads, plain TCP, TLS and short-write paths.
- **HPACK dynamic table churn and lookup** - `HpackDynamicTable::add()` front-inserts into a `vector` (O(n) relocation),
  while `HpackEncoder::findHeader()` linearly scans dynamic entries for every encoded header. Benchmark a circular
  contiguous table, segmented queue and the current vector at realistic 4 KiB and enlarged table sizes; separately test
  an optional name/value index. Select the design on combined insert/evict/indexed-access/lookup results rather than
  assuming a `deque` wins despite its weaker locality.
- **HTTP client response ownership** - HTTP/1.1 identity bodies and assembled HTTP/2 bodies are read into reusable client
  buffers and then copied once by `HttpResponse::body()`. Introduce a safe ownership-transfer or result-owned-buffer path
  that removes the final large-body copy without allowing a later pooled request to invalidate an earlier response.
  Cover identity, chunked, compressed, empty and 1 MiB+ responses. This complements the request-side wire-buffer
  unification already listed in the HTTP client section below.
- **Bound deferred output and zerocopy retention** - audit per-stream HTTP/2 pending data and
  `ConnectionState::zerocopyPendingBuffers` under a peer that stops reading or delays error-queue completions. Add explicit
  high-water marks that pause reads, reject work, or fall back to copied writes rather than allowing retained payloads and
  already-computed responses to grow without a configured bound. Validate memory plateaus under slow-reader tests before
  measuring normal-load overhead.

#### P2 - cache locality, dispatch and repeated scans

- **HTTP/2 stream lookup and hot/cold layout** - extend the stream benchmark beyond insert/erase to randomized lookup,
  DATA/WINDOW_UPDATE processing and close/prune churn at 1, 100, 1,000 and 10,000 active streams. Use the profile to decide
  between the current flat hash map, a tiny recent-stream cache, denser indexing, or splitting frequently touched state
  from callbacks, header storage and other cold fields.
- **Transport dispatch without RTTI probes** - `ITransport` virtualizes every read/write and several server paths also
  `dynamic_cast` to `PlainTransport` or `TlsTransport`. Prototype a tagged final transport/variant or cached capabilities
  (`plain`, `TLS`, `sendfile`, kTLS) and compare cycles, branch misses and binary size on plain and TLS workloads. Adopt it
  only if the gain pays for the larger dispatch surface and preserves optional TLS compilation.
- **HTTP header mutation/search** - `HttpMessage` repeatedly scans the flat CRLF buffer for lookup, append, remove and
  override operations. First profile realistic 4/8/16/32-header construction and middleware mutation. If scans dominate,
  evaluate known-header offsets or a lazy compact index that is invalidated on mutation without adding allocation or
  meaningful `sizeof(HttpMessage)` cost to the common case.
- **TCP cork syscall policy** - Linux response dispatch currently wraps eligible sends in `TcpCorkGuard`, issuing cork and
  uncork `setsockopt` calls even for responses that may already fit in one gathered write. Benchmark disabled, always-on
  and size/write-count-threshold policies for 0 B, 512 B, 4 KiB and streaming responses; retain cork only where packet
  reduction outweighs the extra syscalls and latency.
- **JWT/JWKS verification primitives** - `Jwks::find()` scans every key for each token and base64url decoding classifies
  every character through branches. Add end-to-end JWT decode/verify benchmarks by algorithm, token size and JWKS size,
  then compare linear lookup with a compact sorted/hash index and branch classification with a 256-byte decode table.
  Include missing-`kid` and malformed-input cases so failure paths do not regress.
- **Hot object layout audit** - record `sizeof`, alignment and cache-line access profiles for `ConnectionState`,
  `Http2Stream`, `WebSocketHandler`, `HttpMessage`, `HttpRequestView` and client connection state at 10 K simulated
  connections/streams. Move genuinely cold optional state behind existing natural-empty/pool mechanisms only when cache
  results beat the added indirection. `ConnectionState` lifecycle flags are already bit-packed; do not reopen that item
  without new layout evidence.
- **Compression/decompression retained memory** - benchmark codec context reset, scratch-buffer growth and peak retained
  capacity across alternating tiny/large bodies and many keep-alive sessions. Tune growth/shrink thresholds per codec,
  and continue the Brotli reset/reuse research, only when allocation traces show a benefit over the existing reusable
  `BufferCache` / `ObjectArrayPool` paths.

#### P3 - platform and build experiments

- **Configurable/adaptive HTTP client reads** - the client currently requests fixed 16 KiB chunks for HTTP/1.1 and HTTP/2
  receives. Benchmark configurable 4/16/64 KiB and adaptive growth under small responses, bulk transfers, TLS
  and constrained-memory workloads before exposing a knob or policy.
- **Repeat-connection latency** - add a bounded TTL DNS cache and client-side TLS session reuse only after benchmarks
  separate resolver, TCP and handshake time. Expiry, address rotation and failed-resumption fallback must remain explicit.
- **Profile-guided builds** - evaluate PGO (and BOLT on supported Linux toolchains) in the benchmark binaries against the
  existing Release + IPO baseline. Keep this an opt-in build/documentation path: installed library code must not assume
  the benchmark host's CPU or workload.
- **Platform event backends** - continue the dedicated IOCP, native macOS `EVFILT_TIMER`, and Linux `io_uring` work already
  listed elsewhere in this roadmap. Treat these as transport/backend projects with end-to-end results, not local syscall
  substitutions.

#### Benchmark ideas

Existing `hpack_bench`, `http2_flow_control_bench` and WebSocket mask benchmarks cover basic steady-state operations, so
extend them instead of duplicating them. The WebSocket large-frame SIMD masking gap previously listed here is complete.

| Benchmark gap | Decision it must support |
| --- | --- |
| HPACK insert/evict churn plus hit/miss lookup at multiple table sizes | Dynamic-table container and optional index |
| HTTP/2 DATA + large HEADERS/CONTINUATION output with partial writes | Fragment queue / scatter-write representation |
| Stream randomized lookup, frame processing and prune churn through 10 K streams | Stream map/cache and hot/cold split |
| Client identity/chunked/compressed responses from 0 B through 100 MiB | Response-buffer ownership transfer |
| Static-file cache hit/miss/thrash at capacities 64 through 4,096 | O(1) or amortized eviction policy |
| Transport dispatch and response-size syscall counts, plain + TLS | Tagged transport and TCP cork threshold |
| Header lookup/mutation with 4 through 32 fields | Flat scan versus compact/lazy indexing |
| JWT decode/verify with 1 through 1,000 JWKs | `kid` index and base64url decode strategy |
| Slow-reader and delayed zerocopy-completion memory plateau | Deferred-output high-water marks |
| Codec alternating-size sessions with allocation and peak-capacity counters | Scratch growth/shrink and context reuse |
| Fragmented + compressed WebSocket message processing | End-to-end frame/reassembly path beyond mask-only throughput |
| Hot-structure `sizeof` and 10 K-object random-access profile | Layout changes without speculative padding claims |

## Long-term / Nice-to-have

- **HTTP/3 / QUIC** - a separate transport layer (future major feature); scoped as a research spike below before any commitment.
- **Fuzz Harness Integration** - libFuzzer targets for HTTP/1.1 and HTTP/2 parsing
- **OCSP Stapling & Advanced TLS** - Passive stapling with cached responses, CRL hooks, key logging (debug only)
- **Per-SNI mTLS Policies** - Different client cert requirements per SNI hostname
- **Advanced Metrics** - ~~Histogram/percentile latency buckets~~ ✔ (implemented via `TelemetryContext::histogram()` + `TelemetryConfig::addHistogramBuckets()`); per-route stats not yet implemented

### HTTP/3 / QUIC - research spike

**Not started, and not recommended for the near term.** HTTP/3 is not an increment on the existing stack: it
replaces the transport. QUIC is UDP + user-space loss recovery / congestion control + connection migration +
TLS 1.3 fused into the transport, and it uses **QPACK, not HPACK** - so the current HPACK / frame codecs and
the readiness-based epoll/kqueue `TlsTransport` seam mostly do not carry over (QUIC is more naturally
timer/completion-driven and wants GSO/GRO for throughput). Writing QUIC from scratch is a multi-year,
security-critical effort; the realistic path is wrapping an existing stack behind a new gated transport
module, which trades against aeronet's minimal-heavy-dependency ethos and therefore needs a deliberate
decision rather than a default.

**Higher-leverage work comes first** and reuses the existing architecture: the coroutine client + true HTTP/2
multiplexing, and `io_uring`.

If it becomes strategic, the first step is a time-boxed **design spike** (not a full plan) answering three
questions:

1. **Dependency choice** - `ngtcp2` + `nghttp3` (the curl/nginx pairing), `quiche`, or `lsquic`: build system,
   license, binary-size and API-ergonomics trade-offs.
2. **Event-loop integration** - how a UDP + timer-driven QUIC transport slots into the current readiness-based
   loop (UDP socket handling, per-connection timers, GSO/GRO, pacing).
3. **Handler API reuse** - whether the existing `HttpRequestView` / `Router` / handler surface can sit unchanged on
   top of an HTTP/3 mapping (QPACK encode/decode, request/response streams), so HTTP/3 is a transport swap
   rather than a second application API.

Deliverable of the spike: a go/no-go recommendation with a dependency choice and a rough transport-layer
design, **before** committing to implementation.

### Optional feature modules (compile-time gated)

The following features are inspired by capabilities that make frameworks like **Drogon**, **Crow**, and **Beast** popular for building full applications. Each is designed to be **opt-in via a compile-time flag** (`OFF` by default) so they add zero overhead when unused, consistent with aeronet's modular philosophy.

#### HTTP Client (`AERONET_ENABLE_HTTP_CLIENT`)

Status: **Delivered** (`aeronet/client`, enabled by default). The synchronous `aeronet::HttpClient` speaks
**HTTP/1.1 and HTTP/2** natively over aeronet's own non-blocking transport + event loop, with HTTPS via the
shared `TlsTransport`, per-origin keep-alive pooling, redirect following, a retry + exponential-backoff
policy, transparent response decompression / request compression, cleartext forward-proxy (`CONNECT`
tunneling), and an opt-in time-based response cache for idempotent requests. Every request returns an
`HttpClientResult` (`std::expected<HttpResponse, HttpClientErrc>`) - value-based errors, no throwing on the
request path. See the README HTTP client section and `docs/FEATURES.md` for the full surface.

Example:

```cpp
aeronet::HttpClient client;
auto result = client.get("https://api.example.com/data");
if (result && result->status() == 200) {
  std::string_view body = result->bodyInMemory();
}
```

Still planned:

Client performance work from the repository-wide audit (response-buffer ownership, receive sizing, and DNS/TLS reuse)
is tracked in the [performance optimization backlog](#performance-optimization-backlog) above.

- **Coroutine-friendly API** (`co_await client.get(...)`) integrated with a *running* server event loop, so
  handlers can issue upstream calls without blocking. This is also what unlocks **true HTTP/2 multiplexing**
  in the client: the native HTTP/2 engine has landed, but the synchronous model runs one stream at a time per
  connection.
- **Unify the request buffer with the wire bytes**: teach `HttpMessage` a request-line first-line mode so the
  client builds the request directly in the buffer it writes, with zero reassembly (today request fields are
  copied once into the send buffer).
- **Pluggable timeouts / cancellation** for in-flight client exchanges.
- **Client-side telemetry**: surface pool hit/miss, retry counts, cache hits/misses, redirect hops and
  decompression stats through the same `TelemetryContext` infrastructure the server already uses (the client
  currently emits none).
- **Reverse-proxy / forwarding** building on the client (see the reverse-proxy item below).

#### Server-Sent Events (SSE) convenience layer

Thin abstraction over existing chunked streaming - no new compile flag needed. Auto-formats `text/event-stream` content type with `id:`, `event:`, `data:` field framing and `retry:` reconnect hints. API sugar on `HttpResponseWriter`:

```cpp
router.setPath(http::Method::GET, "/events", [](const HttpRequestView&, HttpResponseWriter& w) {
  SseWriter sse(w);
  sse.event("update", R"({"temp": 22.5})");
  sse.event("update", R"({"temp": 23.1})", "evt-002"); // with id
});
```

Growing in popularity as a lighter alternative to WebSocket for server → client push (dashboards, notifications, live feeds).

#### Rate Limiting middleware (`AERONET_ENABLE_RATE_LIMIT`)

Built-in token bucket or sliding window rate limiter. Per-IP and/or per-route. Returns `429 Too Many Requests` with `Retry-After` header. Configurable burst / sustained rates. Pluggable backend interface (in-memory default, extensible to Redis or shared stores). Currently only TLS handshake rate limiting exists (`TLSConfig::handshakeRateLimitPerSecond`); this would extend to HTTP request level.

Status: **Delivered** (in the next release): middleware-first `RateLimitRequestMiddlewareBuilder` with an in-memory token-bucket backend (`429` + `Retry-After`), a configurable client-key strategy, global or per-route / per-group installation, plus an optional Redis sliding-window contract (`RedisSlidingWindowRateLimitStore`) for distributed synchronization.

#### Cookie helpers & Session store (`AERONET_ENABLE_SESSIONS`)

- **Cookie builder**: RFC 6265-compliant `Set-Cookie` helper with `SameSite`, `Secure`, `HttpOnly`, `MaxAge`, `Path`, `Domain` attributes. Currently only raw header parsing exists.
- **Session store**: In-memory session store with configurable TTL and pluggable backend interface. HMAC-signed session cookie for integrity. Both Drogon and Crow include this built-in; it is the most basic state management primitive for web applications.

#### Response caching middleware (`AERONET_ENABLE_RESPONSE_CACHE`)

In-memory LRU cache keyed by method + path + `Vary` headers. Respects `Cache-Control` directives (`max-age`, `no-store`, `no-cache`, `private`). Configurable max entries and memory budget. Per-route opt-in via middleware registration. ETag / `If-None-Match` validation for cached entries. Useful for APIs with expensive computation behind cacheable endpoints.

#### Regex route constraints

Status: **Delivered in `1.3.0`** as route parameter constraints - inline `{name:pattern}` syntax
(e.g. `/users/{id:[0-9]+}`), compiled at registration, with a fast custom matcher for common character
classes and a `std::regex` fallback. A non-matching parameter yields `404`.

#### Route groups & prefix mounting

Status: Delivered in `1.3.0` (`Router::group`, nested groups, shared middleware/config inheritance).

Organize routes under a common prefix with shared middleware, reducing boilerplate for versioned APIs. Inspired by Express.js `Router`, Gin `Group()`, Axum `Router::nest()`:

```cpp
Router router;
auto api = router.group("/api/v1");
api.addRequestMiddleware(authMiddleware);
api.setPath(http::Method::GET, "/users", listUsersHandler);     // matches /api/v1/users
api.setPath(http::Method::GET, "/users/{id}", getUserHandler);  // matches /api/v1/users/{id}
```

Every major framework provides this (Express, Gin, Axum, Actix-web, FastAPI, Spring Boot). Without it, handler registrations repeat prefixes and middleware is duplicated across routes.

#### Per-route request timeout

Status: **Delivered in `1.3.0`** - `PathHandlerEntry::timeout()` (also settable per group and via JSON/YAML
config) enforces a per-route handler deadline during periodic sweeps (per-connection for HTTP/1.1, per-stream
for HTTP/2), returning `408 Request Timeout` when it expires.

#### Content negotiation (Accept header)

Parse `Accept` header q-values to select response content type (JSON, YAML, XML, plain text). Return `406 Not Acceptable` when no format matches. Currently only `Accept-Encoding` is negotiated; `Accept` (media type) is left to user code. Frameworks like Spring, Rails, Phoenix, and ASP.NET handle this transparently.

```cpp
router.setPath(http::Method::GET, "/data", [](const HttpRequestView& req) {
  auto negotiated = req.negotiate({"application/json", "text/yaml", "text/plain"});
  // returns best match, or std::nullopt → 406
});
```

#### Request ID / correlation ID

Generate a unique ID per request (UUID or monotonic counter), set it on the response (`X-Request-Id`), and forward it to OpenTelemetry spans. If the client provides `X-Request-Id`, the server adopts it. Essential for distributed tracing and log correlation across microservices. Provided by Nginx (`$request_id`), Envoy, Express (`express-request-id`), Spring (`spring-cloud-sleuth`), FastAPI, etc.

#### IP-based access control middleware

Per-route allowlist/denylist by source IP or CIDR range. Returns `403 Forbidden` for denied IPs. Useful for admin endpoints, internal APIs, and compliance. Nginx (`allow/deny`), HAProxy (`acl`), Caddy, Express (`express-ip-filter`), and Go all support this natively or as first-class middleware.

```cpp
IpAccessPolicy policy;
policy.allow("10.0.0.0/8").allow("192.168.1.0/24").denyAll();
router.setPath(http::Method::GET, "/admin", adminHandler).accessPolicy(std::move(policy));
```

#### Authentication helpers (Basic / Bearer / JWT)

**JWT (JWS profile) delivered** (`aeronet/jwt`, `AERONET_ENABLE_JWT`): the full JWS algorithm suite
(HMAC `HS*`, RSA `RS*`/`PS*`, ECDSA `ES*`, `EdDSA`), claim validation (`exp`/`nbf`/`iat`/`iss`/`aud`/`sub`
with leeway + injectable clock), JWK/JWKS parsing with `kid` selection, and the mandatory security posture
(reject `alg:none`, family-based anti-confusion, constant-time HMAC, signature-before-claims). No dedicated
opt-in flag (`cmake_dependent_option`, ON whenever OpenSSL + Glaze are present); JWE out of scope. See
`docs/FEATURES.md` (JWT section) and `aeronet/jwt/test/`.

Still planned: a server-side **middleware** that parses the `Authorization` header (Basic credentials or
Bearer tokens) and wires the JWT verifier into a pluggable validator interface; plus a client-side
**JWKS-fetch + cache** helper building on the HTTP client.

#### Reverse proxy / HTTP forwarding mode

Forward incoming requests to upstream backends, rewriting headers (`X-Forwarded-For`, `X-Forwarded-Proto`, `Via`). Builds naturally on the delivered HTTP client module. Load balancing strategies (round-robin, least-connections). This turns aeronet from a pure application server into an edge/gateway server - a common deployment pattern (Nginx, Caddy, Envoy, Traefik, HAProxy).

#### Inbound request body streaming to handler

Deliver chunked request bodies to handlers as they arrive instead of buffering the full payload first. Enables processing uploads in bounded memory. Currently noted as a limitation in FEATURES.md. Express (streams), Go (`io.Reader`), Axum (`BodyStream`), Actix-web (`Payload`), and Rust Hyper all expose streaming request bodies.

#### Streaming multipart parsing

Process multipart/form-data uploads part-by-part as data arrives, rather than buffering the entire payload. Critical for large file uploads where the full body does not fit in memory. Currently flagged as a future item in FEATURES.md. Popular in Multer (Node.js), Actix-multipart, Spring `StreamingMultipartResolver`, and Go `multipart.Reader`.

#### Per-route body size limits

Status: **Delivered in `1.3.0`** - per-route `maxBodyBytes` / `maxHeaderBytes` overrides on `PathHandlerEntry`
and `RouteGroup`, enforced across HTTP/1.1 (sync + async) and HTTP/2 with `413` / `431`.

#### Graceful config reload via signal (SIGHUP)

Reload the JSON/YAML config file on `SIGHUP` without restarting the server. Apply mutable config changes (timeouts, limits, compression settings, TLS certs) hot. Pairs with the existing Glaze config loader and `postConfigUpdate()`. Nginx, HAProxy, Caddy, and systemd-based services all support `SIGHUP`-driven reload.

#### WebSocket broadcast / pub-sub helpers

Built-in topic-based broadcast for WebSocket connections. `WsRoom` / `WsTopic` abstraction where connections subscribe to topics and the server broadcasts to all subscribers. Common pattern in Socket.IO, uWebSockets, Phoenix Channels, and SignalR. Reduces boilerplate for chat, dashboards, and real-time notification use cases.

#### ETag support for dynamic responses

Compute a weak or strong ETag from the response body (e.g. hash) and handle `If-None-Match` / `304 Not Modified` for dynamic endpoints, not just static files. Reduces bandwidth for API responses that rarely change. Express (`etag` by default), Rails, ASP.NET, and Spring all auto-generate ETags for regular responses.

### TLS enhancements (detailed roadmap)

#### Phase 3 (Advanced / Enterprise)

- OCSP stapling (passive, cached)
- Optional CRL / revocation hooks
- Key log (debug only)
- Security hardening audits (zeroization, memory scrub confirmations)

#### Phase 4 (Future Protocol / Extensibility)

- Per-SNI mTLS policies
- Session ticket key rotation scheduling & multi-key window
- (Stretch) Exploring QUIC/HTTP/3 (would likely be a separate transport layer, so only mention if strategic)

## Realistic Network Testing

Goals

- Improve confidence that aeronet behaves correctly under latency, jitter, packet loss, reordering, partial delivery, and connection resets.
- Catch protocol-level bugs (HTTP/1.1 and HTTP/2), TLS handshake fragility, flow-control and resource-leak issues that do not appear on a perfect local loopback.

Approach and Components

- ✅ **Deterministic simulated-network unit tests (DONE)**:
  - `TestPipe` + `TestTransport`: in-memory transport with `FaultPolicy` for deterministic partial reads/writes, EAGAIN, and connection resets.
  - `FaultInjectingTransport`: decorator wrapping real transports for integration tests with the full event loop.
  - Transport test hook (`g_transportDecorator`): compile-time gated (`AERONET_ENABLE_TEST_HOOKS`) global atomic that decorates transports at accept time.
  - 26 unit tests + 11 integration tests covering partial delivery, EAGAIN, resets, combined faults, pipelining.
  - Already found and fixed a latent HTTP/1.1 parser bug (header boundary off-by-one).

- Proxy-based user-space fault injection (medium priority):
  - Integrate Toxiproxy or a small custom proxy harness for tests that exercise full binaries without requiring NET_ADMIN privileges.
  - Use the proxy to inject latency, connection cuts, truncation and partial writes to validate end-to-end behaviors.

- Kernel-level integration using `tc netem` and network namespaces (lower priority):
  - Create e2e tests that run client & server in separate Linux network namespaces connected by a veth pair with `tc netem` rules applied.
  - Simulate real TCP behaviours (retransmits, delayed ACKs, segment coalescing, zero-window events) that only the kernel stack exhibits.
  - These tests are heavier and run in nightly or scheduled CI only; they are optional for PRs because they require privileged runners and can be flaky.

Test Design & Best Practices

- Start with syscall error injection tests (EINTR, EAGAIN, EPIPE, ECONNRESET) and partial I/O.
- Prefer deterministic `TestTransport` unit tests for core protocol logic: easier to reproduce and debug.
- For integration tests, capture detailed artifacts on failures: pcap, logs, and deterministic seeds used by the test harness.
- Expose test-time configuration hooks (shorter timeouts, deterministic timers) so tests run quickly and reliably.

CI Policy

- PRs: run all deterministic unit tests (including `TestTransport` simulated-network tests) and integration fault-injection tests.
- Nightly: run proxy-based and `tc netem` integration suites; mark these jobs non-blocking for PRs.

Milestones

1. ✅ Add `TestTransport` abstraction and deterministic unit tests covering partial I/O, EAGAIN, and resets.
2. ✅ Add `FaultInjectingTransport` + global hook for integration tests with real sockets.
3. ✅ Integration tests for HTTP/1.1 under faults (partial reads/writes, resets, pipelining).
4. Add HTTP/2 direct protocol tests using `TestTransport` + `processInput()` for frame-level fault injection.
5. Add simple Toxiproxy-based harness and a handful of end-to-end tests (connection cut, high latency, truncated responses).
6. Add `ip netns` + `tc netem` scripts in `tests/e2e/` and integrate nightly CI job.
7. Collect failure artifacts (pcap + logs) and add tooling to reproduce failing scenarios locally with the same netem parameters.

Known Limitations

- **Read-EAGAIN incompatible with EPOLLET in integration tests**: When `FaultInjectingTransport` returns `{0, ReadReady}` (simulated EAGAIN), the server breaks to wait for a new EPOLLIN edge. But with EPOLLET, the socket already has data so no edge fires - causing a hang. Workaround: EAGAIN on reads is tested only at unit level; integration tests use partial reads (which work because `EPOLL_CTL_ADD` with existing data triggers an initial event).

Acceptance Criteria

- Protocol correctness under simulated faults: no state corruption, proper error propagation, and graceful teardown.
- No resource leaks (sockets, memory) in faulted runs.
- Deterministic unit tests reproduce failures locally with a seed and do not rely on privileged resources.

Notes

- The transport test hook is compile-time gated (`#ifdef AERONET_ENABLE_TEST_HOOKS`) - zero cost in production builds.
- Proxy-based tests are useful when privileged operations are not available in CI.
