# aeronet Roadmap - Planned / Not Implemented

## High priority

- **Security Hardening Audit**:
  - TLS fingerprinting hardening (avoid leaking version/cipher info in edge cases)
  - Memory scrubbing for sensitive data (handshake keys, session tickets)
  - Fuzzing harness integration (libFuzzer + AFL)

## Medium priority

- **Windows event loop performance**: The Windows backend uses WSAPoll (readiness‑based, like epoll/kqueue) which is functionally correct but less performant than IOCP for high‑concurrency workloads. A future IOCP backend would require a fundamental architecture shift from readiness to completion semantics.
- **macOS `EVFILT_TIMER` integration**: `TimerFd::armPeriodic()` on macOS is currently a no-op and relies on poll timeouts. Using kqueue's native `EVFILT_TIMER` would improve timer precision but requires event-loop refactoring to accommodate heterogeneous kqueue filter types.
- **Pluggable logging sink API (non-access logs)** - spdlog backend supports custom sinks/formatters; an aeronet-native sink registration API is not yet exposed.
- **Enhanced parser diagnostics** (byte offset in parse errors for better debugging)
- **Direct compression option for HEAD**: optional config to allow HEAD responses to match GET headers
  (Content-Encoding + compressed Content-Length) when desired.
- **Performance improvements**:
  - Further hot-path cache locality optimization
- Enhance `telemetry` with more detailed HTTP/2 metrics: per-stream stats, HPACK compression ratios, frame type distributions.
  - Support tags/labels for metrics

### Performance improvement ideas

#### Low priority / specialized

- **`ConnectionState` bool bit-packing** - 10 scattered `bool` fields + `CloseMode` / `ProtocolType` enums waste bytes to padding between larger fields. Pack booleans into a `uint16_t` bitfield, or reorder all fields by descending size. Saves ~16–32 bytes per `ConnectionState` - meaningful at 10 K+ connections.
- **Brotli encoder context reuse across sessions** - Brotli state is destroyed and recreated each session, unlike Zstd (`ZSTD_CCtx_reset()`) and Zlib (stream reuse). Explore caching the `BrotliEncoderState` and resetting parameters between sessions, or a lighter reinit pattern.
- Enforce backpressure correctness to avoid overload and wasted work.
- **Pre-computed static file response headers** - response headers (`Content-Type`, `Content-Length`, `ETag`, `Last-Modified`) are formatted per request for the same file. Cache fully‑formed header bytes alongside file metadata; invalidate on stat change.
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

## Long-term / Nice-to-have

- **HTTP/3 / QUIC Support** - Likely separate transport layer implementation (future major feature)
- **Fuzz Harness Integration** - libFuzzer targets for HTTP/1.1 and HTTP/2 parsing
- **OCSP Stapling & Advanced TLS** - Passive stapling with cached responses, CRL hooks, key logging (debug only)
- **Per-SNI mTLS Policies** - Different client cert requirements per SNI hostname
- **Advanced Metrics** - ~~Histogram/percentile latency buckets~~ ✔ (implemented via `TelemetryContext::histogram()` + `TelemetryConfig::addHistogramBuckets()`); per-route stats not yet implemented

### Optional feature modules (compile-time gated)

The following features are inspired by capabilities that make frameworks like **Drogon**, **Crow**, and **Beast** popular for building full applications. Each is designed to be **opt-in via a compile-time flag** (`OFF` by default) so they add zero overhead when unused, consistent with aeronet's modular philosophy.

#### HTTP Client (`AERONET_ENABLE_HTTP_CLIENT`)

Status: **Initial HTTP/1.1 client delivered** (`aeronet/client`, enabled by default via `AERONET_ENABLE_HTTP_CLIENT`).

Delivered in the first iteration (`aeronet::HttpClient`):

- Synchronous request API over aeronet's **non-blocking** transport + event-loop bricks (`ConnectTCP`, `EventLoop`, `PlainTransport`, `TlsTransport`), so no extra I/O stack is introduced.
- Plain HTTP and HTTPS (the latter reusing the shared OpenSSL `TlsTransport`, with SNI + optional peer/hostname verification, gated on `AERONET_ENABLE_OPENSSL`).
- Per-origin **keep-alive connection pooling** with bounded idle reuse, plus a configurable **retry + exponential-backoff** policy (`RetryConfig`): an always-on, budget-free transparent retry on a stale pooled connection (pre-send only), and opt-in backoff retries for connect failures, retryable statuses (`429`/`503`, honoring `Retry-After`) and idempotent post-send failures.
- Response parsing for `Content-Length`, **chunked** transfer-encoding (extensions + trailers tolerated), and connection-close framing; 1xx interim responses discarded.
- Automatic **redirect following** (301/302/303/307/308) with method/body rewriting per RFC 7231 and a configurable hop limit.
- Convenience verbs (`get`/`head`/`post`/`put`/`del`) plus a fluent `ClientRequest` builder, returning an `HttpClientResult` (`std::expected<HttpResponse, HttpClientErrc>`) over the existing aeronet response type (reused as the message container).
- **Value-based error model**: per-request runtime failures (invalid URL, connect failure, timeout, TLS error, malformed/oversized response, ...) are reported as an `HttpClientErrc` in the result's error state — never thrown. Exceptions stay reserved for hard setup errors (client / TLS-context misconfiguration). A non-2xx status is a normal `HttpResponse` in the success state.
- **Automatic response decompression** (`gzip`/`deflate`/`br`/`zstd`, gated on compiled-in codecs) and optional **request body compression** for large payloads, reusing the server's codec bricks and decoding without an extra copy of the compressed bytes. Driven by `HttpClientConfig::decompression` / `requestCompression` (auto-advertises a default `Accept-Encoding` when decompression is enabled).

Example:

```cpp
aeronet::HttpClient client;
auto result = client.get("https://api.example.com/data");
if (result && result->status() == 200) {
  std::string_view body = result->bodyInMemory();
}
```

Next steps (not yet implemented):

- **Unify the request buffer with the wire bytes**: `HttpMessage` is a flat, HTTP/1.1-optimised
  buffer that, for responses, already IS the exact bytes written to the socket. Teaching it a
  request-line first-line mode ("METHOD target HTTP/1.1") would let the client build the request
  directly in that buffer and write it with zero reassembly (today the request fields live in an
  `HttpMessage` and are copied once into the send buffer).
- **Coroutine-friendly API** (`co_await client.get(...)`) and integration with a *running* server event loop so handlers can issue upstream calls without blocking. The current client owns its own loop and drives it synchronously.
- **Native HTTP/2 client** (reusing the existing HPACK + frame codecs and `Http2ProtocolHandler`), including ALPN negotiation. *Groundwork landed:* the wire protocol now sits behind the `internal::ClientConnection`, each pooled connection carries a `ClientProtocol` tag, the client advertises/reads ALPN over HTTPS, and the pool no longer assumes a 1:1 connection↔request model. Remaining work is the HTTP/2 engine itself: advertise `h2`, add the `ensureProtocolHandler` branch, and a multiplexing-aware pool path (`ClientConnection::canTakeAnotherStream`).
- **Zero-copy body delivery** for identity responses (currently the body is copied once into the result), and pluggable timeouts/cancellation.
- **Reverse-proxy / forwarding** building on the client (see the reverse-proxy item below).

> Server-side prerequisite for efficient client keep-alive: aeronet currently emits empty-body
> responses (e.g. a bare `302` redirect with only a `Location` header) **without** a
> `Content-Length: 0` or `Transfer-Encoding` header. Per RFC 7230 §3.3.3 a compliant client must
> then treat the body as ending at connection close, which defeats keep-alive reuse for such
> responses (the client waits for the idle timeout). Emitting `Content-Length: 0` for empty-body
> HTTP/1.1 responses (as nginx/Apache do) would let all keep-alive clients reuse the connection
> immediately. Tracked as a follow-up server improvement.

#### Server-Sent Events (SSE) convenience layer

Thin abstraction over existing chunked streaming - no new compile flag needed. Auto-formats `text/event-stream` content type with `id:`, `event:`, `data:` field framing and `retry:` reconnect hints. API sugar on `HttpResponseWriter`:

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

Status: Initial middleware-first implementation is now available with in-memory token bucket support and a Redis-gated sliding-window store stub for distributed synchronization adapters.

#### Cookie helpers & Session store (`AERONET_ENABLE_SESSIONS`)

- **Cookie builder**: RFC 6265-compliant `Set-Cookie` helper with `SameSite`, `Secure`, `HttpOnly`, `MaxAge`, `Path`, `Domain` attributes. Currently only raw header parsing exists.
- **Session store**: In-memory session store with configurable TTL and pluggable backend interface. HMAC-signed session cookie for integrity. Both Drogon and Crow include this built-in; it is the most basic state management primitive for web applications.

#### Response caching middleware (`AERONET_ENABLE_RESPONSE_CACHE`)

In-memory LRU cache keyed by method + path + `Vary` headers. Respects `Cache-Control` directives (`max-age`, `no-store`, `no-cache`, `private`). Configurable max entries and memory budget. Per-route opt-in via middleware registration. ETag / `If-None-Match` validation for cached entries. Useful for APIs with expensive computation behind cacheable endpoints.

#### Regex route constraints

Optional regex validation on path parameters, extending the existing radix tree router:

```cpp
router.setPath(http::Method::GET, "/users/{id:[0-9]+}", handler);       // digits only
router.setPath(http::Method::GET, "/files/{path:[a-zA-Z0-9/._-]+}", handler); // safe path chars
```

Returns 404 on constraint mismatch. Consider [CTRE](https://github.com/hanickadot/compile-time-regular-expressions) for compile-time regex performance, with `std::regex` fallback. Drogon supports regex routing natively.

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

Enforce a deadline on handler execution so slow handlers do not hold connections indefinitely. Returns `504 Gateway Timeout` (or configurable status) when the deadline expires. Integrates naturally with coroutines (`co_await` cancellation). Currently only TLS handshake and header-read timeouts exist; this covers the application handler layer.

```cpp
router.setPath(http::Method::GET, "/slow", handler)
      .timeout(std::chrono::seconds{5});
```

Go (`context.WithTimeout`), Axum (`tower::timeout`), Actix-web (`web::Timeout`) and Spring (`@Transactional(timeout=)`) all provide this. Critical for production resilience.

#### Content negotiation (Accept header)

Parse `Accept` header q-values to select response content type (JSON, YAML, XML, plain text). Return `406 Not Acceptable` when no format matches. Currently only `Accept-Encoding` is negotiated; `Accept` (media type) is left to user code. Frameworks like Spring, Rails, Phoenix, and ASP.NET handle this transparently.

```cpp
router.setPath(http::Method::GET, "/data", [](const HttpRequest& req) {
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

**JWT (JWS profile) delivered** (`aeronet/jwt`, `AERONET_ENABLE_JWT`): standalone module for signing and
verifying JSON Web Tokens (RFC 7519). It covers the full JWS algorithm suite (HMAC `HS*`, RSA `RS*`/`PS*`,
ECDSA `ES*`, `EdDSA`), claim validation (`exp`/`nbf`/`iat`/`iss`/`aud`/`sub` with leeway + injectable clock),
JWK/JWKS parsing with `kid` selection, and the mandatory security posture (reject `alg:none`, family-based
anti-confusion, constant-time HMAC, signature-before-claims). Design decisions: **no dedicated opt-in flag** —
it is a `cmake_dependent_option` defaulting ON whenever `AERONET_ENABLE_OPENSSL` + `AERONET_ENABLE_GLAZE` are
present (it reuses their crypto + JSON, adding no new dependency); **JWE (encryption) is out of scope**. See
`docs/FEATURES.md` (JWT section) and `aeronet/jwt/test/`.

Still planned: a server-side **middleware** that parses the `Authorization` header, extracts Basic credentials
or Bearer tokens, and wires the JWT verifier into a pluggable validator interface; plus a client-side
**JWKS-fetch + cache** helper building on the HTTP client. Every web framework provides at least Basic/Bearer
auth middleware (Express `passport`, Axum `axum-extra`, Gin `gin-jwt`, Spring Security, ASP.NET Identity).

#### Reverse proxy / HTTP forwarding mode

Forward incoming requests to upstream backends, rewriting headers (`X-Forwarded-For`, `X-Forwarded-Proto`, `Via`). Pairs naturally with the planned HTTP Client module. Load balancing strategies (round-robin, least-connections). This turns aeronet from a pure application server into an edge/gateway server - a common deployment pattern (Nginx, Caddy, Envoy, Traefik, HAProxy).

#### Inbound request body streaming to handler

Deliver chunked request bodies to handlers as they arrive instead of buffering the full payload first. Enables processing uploads in bounded memory. Currently noted as a limitation in FEATURES.md. Express (streams), Go (`io.Reader`), Axum (`BodyStream`), Actix-web (`Payload`), and Rust Hyper all expose streaming request bodies.

#### Streaming multipart parsing

Process multipart/form-data uploads part-by-part as data arrives, rather than buffering the entire payload. Critical for large file uploads where the full body does not fit in memory. Currently flagged as a future item in FEATURES.md. Popular in Multer (Node.js), Actix-multipart, Spring `StreamingMultipartResolver`, and Go `multipart.Reader`.

#### Per-route body size limits

Different `maxBodyBytes` on different endpoints (e.g. `/upload` allows 100 MB, `/api` allows 1 MB). Currently only a global limit exists. Nginx (`client_max_body_size` per `location`), Express (`express.json({limit})`), Spring (`@RequestMapping` with size),  and Gin all support per-route limits.

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

- **Read-EAGAIN incompatible with EPOLLET in integration tests**: When `FaultInjectingTransport` returns `{0, ReadReady}` (simulated EAGAIN), the server breaks to wait for a new EPOLLIN edge. But with EPOLLET, the socket already has data so no edge fires — causing a hang. Workaround: EAGAIN on reads is tested only at unit level; integration tests use partial reads (which work because `EPOLL_CTL_ADD` with existing data triggers an initial event).

Acceptance Criteria

- Protocol correctness under simulated faults: no state corruption, proper error propagation, and graceful teardown.
- No resource leaks (sockets, memory) in faulted runs.
- Deterministic unit tests reproduce failures locally with a seed and do not rely on privileged resources.

Notes

- The transport test hook is compile-time gated (`#ifdef AERONET_ENABLE_TEST_HOOKS`) — zero cost in production builds.
- Proxy-based tests are useful when privileged operations are not available in CI.
