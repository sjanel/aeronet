# aeronet Roadmap - Planned / Not Implemented

## High priority

- **Security Hardening Audit**:
  - TLS fingerprinting hardening (avoid leaking version/cipher info in edge cases)
  - Fuzzing harness integration (libFuzzer + AFL)

## Medium priority

- **Pluggable logging sink API (non-access logs)** - spdlog backend supports custom sinks/formatters; an aeronet-native sink registration API is not yet exposed.
- **Enhanced parser diagnostics** (byte offset in parse errors for better debugging)
- **Manage TE header in the client**: currently the TE header is reserved in the framework, so it cannot be sent. It could be a good idea to have a flag in the config to allow sending TE headers in the client with trailers essentially (because trailers are natively supported by aeronet). If `TE: trailers` is sent, in HTTP/1.1 we also need to add `Connection: TE`.
- **Protocol Buffers, next steps** (the binary and ProtoJSON bodies, and the choice of their encoding from `Accept`, are [delivered](modules/protobuf.md)):
  - ProtoJSON options (`ignore_unknown_fields`, printing the fields at their default value) and deterministic binary serialization, behind an aeronet options struct (a server-wide `ProtobufConfig`, loadable from configuration files), since their names differ between protobuf versions;
  - streaming: length-delimited message streams over `HttpResponseWriter` and `readBody()`;
  - Connect unary (a plain `POST` with `application/proto`), then gRPC unary over HTTP/2 (5-byte message framing, `grpc-status` trailers, `grpc-encoding`, `grpc-timeout`).

## Low priority

- **Windows event loop performance**: The Windows backend uses WSAPoll (readiness‑based, like epoll/kqueue) which is functionally correct but less performant than IOCP for high‑concurrency workloads. A future IOCP backend would require a fundamental architecture shift from readiness to completion semantics.
- **Encoded slash handling option**: an encoded slash (`%2F`) in the request path is always answered `400`. Some APIs put one in an identifier (npm scoped packages `/@scope%2fname`, GitLab `/projects/group%2Fproject`), and Tomcat, Apache and Spring Security let it through on demand. A parsing setting in `HttpServerConfig` (not `RouterConfig`: the path is decoded before routing, and `req.path()` is also read by middleware and static files) could add a `Keep` mode leaving `%2F` encoded in `req.path()`, so that `/{id}` captures `group%2Fproject`. It must then also keep `%25` encoded, otherwise `/a%252Fb` and `/a%2Fb` give the same path. A `Decode` mode (the former behavior) would only help catch-all and static routes.
- **macOS `EVFILT_TIMER` integration**: `TimerFd::armPeriodic()` on macOS is currently a no-op and relies on poll timeouts. Using kqueue's native `EVFILT_TIMER` would improve timer precision but requires event-loop refactoring to accommodate heterogeneous kqueue filter types.

### Performance optimization backlog

Status: repository-wide source audit completed on 2026-07-22; the opportunities below are not yet implemented or
proven faster. They are ordered by expected leverage, not by unverified percentage estimates.

#### P2 - cache locality, dispatch and repeated scans

- **HTTP/2 stream lookup and hot/cold layout** - extend the stream benchmark beyond insert/erase to randomized lookup, DATA/WINDOW_UPDATE processing and close/prune churn at 1, 100, 1,000 and 10,000 active streams. Use the profile to decide between the current flat hash map, a tiny recent-stream cache, denser indexing, or splitting frequently touched state from callbacks, header storage and other cold fields.
- **HTTP header mutation/search** - `HttpMessage` repeatedly scans the flat CRLF buffer for lookup, append, remove and override operations. First profile realistic 4/8/16/32-header construction and middleware mutation. If scans dominate, evaluate known-header offsets or a lazy compact index that is invalidated on mutation without adding allocation or meaningful `sizeof(HttpMessage)` cost to the common case.
- **TCP cork syscall policy** - Linux response dispatch wraps a flush in `TcpCorkGuard` only when it takes several writes (a response head then its file, output queued around a file, user-space TLS output longer than one record). Measure the packets and syscalls of these cases against no cork - a small file after its head costs two `setsockopt` calls to save one partial segment - and keep cork only where packet reduction outweighs the extra syscalls and latency.
- **JWT/JWKS verification primitives** - `Jwks::find()` scans every key for each token and base64url decoding classifies every character through branches. Add end-to-end JWT decode/verify benchmarks by algorithm, token size and JWKS size, then compare linear lookup with a compact sorted/hash index and branch classification with a 256-byte decode table. Include missing-`kid` and malformed-input cases so failure paths do not regress.
- **Hot object layout audit** - record `sizeof`, alignment and cache-line access profiles for `ConnectionState`, `Http2Stream`, `WebSocketHandler`, `HttpMessage`, `HttpRequestView` and client connection state at 10 K simulated connections/streams. Move genuinely cold optional state behind existing natural-empty/pool mechanisms only when cache results beat the added indirection. `ConnectionState` lifecycle flags are already bit-packed; do not reopen that item without new layout evidence.
- **Compression/decompression retained memory** - benchmark codec context reset, scratch-buffer growth and peak retained capacity across alternating tiny/large bodies and many keep-alive sessions. Tune growth/shrink thresholds per codec, and continue the Brotli reset/reuse research, only when allocation traces show a benefit over the existing reusable `BufferCache` / `ObjectArrayPool` paths.

#### P3 - platform and build experiments

- **Configurable/adaptive HTTP client reads** - the client currently requests fixed 16 KiB chunks for HTTP/1.1 and HTTP/2 receives. Benchmark configurable 4/16/64 KiB and adaptive growth under small responses, bulk transfers, TLS and constrained-memory workloads before exposing a knob or policy.
- **Repeat-connection latency** - add a bounded TTL DNS cache and client-side TLS session reuse only after benchmarks separate resolver, TCP and handshake time. Expiry, address rotation and failed-resumption fallback must remain explicit.
- **Profile-guided builds** - evaluate PGO (and BOLT on supported Linux toolchains) in the benchmark binaries against the existing Release + IPO baseline. Keep this an opt-in build/documentation path: installed library code must not assume the benchmark host's CPU or workload.
- **Platform event backends** - continue the dedicated IOCP, native macOS `EVFILT_TIMER`, and Linux `io_uring` work already listed elsewhere in this roadmap. Treat these as transport/backend projects with end-to-end results, not local syscall substitutions.

#### Benchmark ideas

Existing `hpack_bench`, `http2_flow_control_bench` and WebSocket mask benchmarks cover basic steady-state operations, so
extend them instead of duplicating them. The WebSocket large-frame SIMD masking gap previously listed here is complete.

| Benchmark gap | Decision it must support |
| --- | --- |
| Stream randomized lookup, frame processing and prune churn through 10 K streams | Stream map/cache and hot/cold split |
| Client identity/chunked/compressed responses from 0 B through 100 MiB | Response-buffer ownership transfer |
| Static-file cache hit/miss/thrash at capacities 64 through 4,096 | O(1) or amortized eviction policy |
| Response-size syscall counts, plain + TLS | TCP cork threshold |
| Header lookup/mutation with 4 through 32 fields | Flat scan versus compact/lazy indexing |
| JWT decode/verify with 1 through 1,000 JWKs | `kid` index and base64url decode strategy |
| Codec alternating-size sessions with allocation and peak-capacity counters | Scratch growth/shrink and context reuse |
| Fragmented + compressed WebSocket message processing | End-to-end frame/reassembly path beyond mask-only throughput |
| Hot-structure `sizeof` and 10 K-object random-access profile | Layout changes without speculative padding claims |

## Long-term / Nice-to-have

- **HTTP/3 / QUIC** - a separate transport layer (future major feature); scoped as a research spike below before any commitment.
- **Fuzz Harness Integration** - libFuzzer targets for HTTP/1.1 and HTTP/2 parsing
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

Deliverable of the spike: a go/no-go recommendation with a dependency choice and a rough transport-layer design, **before** committing to implementation.

### Optional feature modules (compile-time gated)

The following features are inspired by capabilities that make frameworks like **Drogon**, **Crow**, and **Beast** popular for building full applications. Each is designed to be **opt-in via a compile-time flag** (`OFF` by default) so they add zero overhead when unused, consistent with aeronet's modular philosophy.

#### HTTP Client (`AERONET_ENABLE_HTTP_CLIENT`)

Status: **Delivered** (`aeronet/client`, enabled by default). The synchronous `aeronet::HttpClient` speaks **HTTP/1.1 and HTTP/2** natively over aeronet's own non-blocking transport + event loop, with HTTPS via the shared `TlsTransport`, per-origin keep-alive pooling, redirect following, a retry + exponential-backoff policy, transparent response decompression / request compression, cleartext forward-proxy (`CONNECT` tunneling), and an opt-in time-based response cache for idempotent requests. Every request returns an `HttpClientResult` (`std::expected<HttpResponse, HttpClientErrc>`) - value-based errors, no throwing on the request path. See the [HTTP client guide](modules/http-client.md) for the full surface.

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

- **Coroutine-friendly API** (`co_await client.get(...)`) integrated with a *running* server event loop, so handlers can issue upstream calls without blocking. This is also what unlocks **true HTTP/2 multiplexing** in the client: the native HTTP/2 engine has landed, but the synchronous model runs one stream at a time per connection.
- **Unify the request buffer with the wire bytes**: teach `HttpMessage` a request-line first-line mode so the client builds the request directly in the buffer it writes, with zero reassembly (today request fields are copied once into the send buffer).
- **Cancellation from another thread** of an in-flight client exchange (a `ResponseBodySink` can already stop a transfer, and `inactivityTimeout` bounds a stalled one).
- **Client-side telemetry**: surface pool hit/miss, retry counts, cache hits/misses, redirect hops and decompression stats through the same `TelemetryContext` infrastructure the server already uses (the client currently emits none).
- **Cookie jar** (RFC 6265): store `Set-Cookie` per origin and send `Cookie` back (domain/path matching, `Secure`, `HttpOnly`, expiry, `SameSite`), opt-in in `HttpClientConfig`. Today a redirect chain or a multi-request flow loses session cookies unless the caller copies them by hand. Prerequisite of the [JavaScript pages](#html-parsing-and-javascript-pages-aeronet_enable_html-aeronet_enable_js) phases.
- **Reverse-proxy / forwarding** building on the client (see the reverse-proxy item below).

#### HTML parsing and JavaScript pages (`AERONET_ENABLE_HTML`, `AERONET_ENABLE_JS`)

Status: **phase 1 delivered** ([HTML parsing](modules/html.md)), JavaScript not started
([#738](https://github.com/sjanel/aeronet/issues/738)). Goal: let the client read pages whose content is produced or
completed by JavaScript, e.g. `page.querySelectorAll("table tr")` after a `navigate()`.

**The JS engine is the small part.** Embedding one is a few C files; the DOM, the script loading rules, the event
loop and the Web APIs (`fetch`, XHR, timers, `location`, storage, observers...) are most of the work, and that
surface is never complete: Lightpanda, a funded headless browser built on V8, still describes its Web API coverage
as partial. On the other hand many "JavaScript pages" do not need JS at all: they are rendered on the server, or
they embed their data as JSON that the page scripts turn into markup.

Worked example, the [Kraken withdrawal fees page](https://support.kraken.com/articles/360000767986-cryptocurrency-withdrawal-fees-and-minimums)
(checked 2026-11-06): the served HTML (4 MB) has no `<table>`, and the page loads 51 external scripts (2.8 MB of
JS: React, code-split chunks, consent and analytics). The rows are in an inline script
`window.__INITIAL_PROPS__ = {...}` (a TanStack Query dehydrated state): its `WITHDRAWAL_METHODS_SUBSCRIBER` query
holds 799 entries with `asset_name`, `asset`, `fee`, `min_amount` and the network, from which React builds the
table. Phase 1 reads them without running any script. The fees are recomputed between requests, so values copied
from a screenshot are not found later by `grep`.

Dependencies (C libraries, kept out of the public headers):

- **lexbor** (Apache-2.0, C99, CMake, vcpkg and Conan): WHATWG HTML parser, DOM, CSS selectors, encodings and URL
  parser, also the HTML5 parser of PHP 8.4. Version 3.0 adds per-element mutation callbacks (insertion, removal,
  attribute change), the hook for scripts inserted at runtime and `MutationObserver`; each node has a `user`
  pointer to cache its JS wrapper (the one of the document node holds the query engine of `HtmlDocument`).
- **QuickJS-ng** (MIT, ES2023+, CMake, MSVC, vcpkg): not in Conan Center, which only has Bellard's QuickJS -
  either contribute a recipe or support both behind a thin layer (their C APIs are close). No `Intl` (the
  project says it will likely never have it, so i18n libraries need stubs), no WebAssembly, interpreter speed.
  V8, SpiderMonkey and JavaScriptCore are rejected for binary size and build complexity; Duktape, MuJS and
  JerryScript cannot parse modern bundles (ES5).

Phases:

1. **HTML** - delivered, see [HTML parsing](modules/html.md): `HtmlDocument` (encoding sniffing, read-only DOM),
   CSS selector queries (as text or compiled `CssSelector`), and `assignedJson()` for the state embedded by
   server-rendered pages. Still open: DOM mutation and serialization of a modified tree (needed by phase 3 anyway).
   The fragment parsing behind `innerHTML` and `insertAdjacentHTML` must get the same nesting limits as the document
   parsing (`aeronet/html/src/html-parse.cpp`).
2. **Client cookie jar** (see the HTTP client items above), needed by any multi-request navigation and by
   `document.cookie`.
3. **JS core** (`AERONET_ENABLE_JS`, requires `AERONET_ENABLE_HTML` and `AERONET_ENABLE_HTTP_CLIENT`, OFF by
   default), module `aeronet/web`: `WebClient::navigate(url, options)` returning a `std::expected`, the same
   `Element` API as phase 1, and `evaluate()`.
   - One QuickJS runtime per page over the lexbor document; the page owns every node and frees them together,
     so node lifetime does not depend on the JS garbage collector.
   - Scripts run in document order with `defer` semantics after a full parse (`document.write` during parsing
     is limited).
   - Event loop: one task, then the microtasks (`JS_ExecutePendingJob`). Timers use a virtual clock: they fire
     in order without sleeping, with caps on far timers and on `setInterval` runs.
   - `fetch` and XHR go through the page's `HttpClient` (synchronous XHR comes for free).
   - `navigate()` returns on network idle (no request in flight, no timer due), on a matching selector, or when
     the time budget runs out (QuickJS interrupt handler).
4. **Single-page app compatibility**: ES modules and `import()`, scripts inserted at runtime (webpack chunk
   loading) through the lexbor mutation callbacks, `MutationObserver`, stubs (`Intl`, `matchMedia`,
   `getComputedStyle`, `IntersectionObserver` reporting everything visible, layout sizes of 0),
   `waitForSelector()`, then clicks and form input.
5. **Optional, independent**: a driver for full-fidelity browsers (Chrome, Lightpanda) over CDP (JSON over
   WebSocket). Little aeronet code, but it needs a WebSocket client, which aeronet does not have.

```cpp
// Phase 1 - no JS (delivered)
auto resp = client.get(url);
aeronet::HtmlDocument doc(resp->bodyInMemory(), resp->headerValueOrEmpty(aeronet::http::ContentType));
for (aeronet::HtmlElement row : doc.querySelectorAll("table tr")) {
  std::string text = row.textContent();
}
std::string_view state = doc.assignedJson("window.__INITIAL_PROPS__");  // parse with glaze

// Phase 3 - with JS
aeronet::WebClient web;
auto page = web.navigate(url, {.until = aeronet::LoadState::NetworkIdle, .timeout = std::chrono::seconds{10}});
if (page) {
  auto rows = page->querySelectorAll("table tr");
}
```

Scope: server-rendered pages with JS on top, and single-page apps (React, Vue, Svelte, webpack) that only need the
DOM, `fetch` / XHR and timers. Out of scope: rendering and layout, anti-bot challenges (they fingerprint canvas,
WebGL and timing), WebAssembly, workers, iframes, canvas, media.

Performance: the synchronous client fetches the subresources one at a time (51 sequential exchanges in the example
above); the coroutine client with HTTP/2 multiplexing (above) is the fix. Measure the QuickJS-ng compile and run
time of real bundles before committing to phase 3.

Security: `navigate()` runs remote code in the host process, and QuickJS-ng had memory-safety CVEs in 2025-2026
(typed arrays, `Atomics`, iterator helpers). Never link `quickjs-libc` (no file or OS access); set memory and stack
limits and a CPU budget; add a network policy (allowed origins, request and byte caps); leave out
`SharedArrayBuffer` and `Atomics`; pin the engine version, fuzz the bindings and run the JS tests in the ASAN CI
legs. Document that untrusted sites belong in an unprivileged process; an out-of-process sandbox (seccomp,
landlock) can come later.

Tests: pages served by aeronet's own `HttpServer` (a webpack-style chunk loader, a vendored production React build
rendering a table), so CI needs no internet.

Open decisions: whether this scope is enough or the CDP driver should come first, the Conan strategy for the
engine, and in-process only versus a sandbox from the start.

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

#### Cookie helpers & Session store (`AERONET_ENABLE_SESSIONS`)

- **Cookie builder**: RFC 6265-compliant `Set-Cookie` helper with `SameSite`, `Secure`, `HttpOnly`, `MaxAge`, `Path`, `Domain` attributes. Currently only raw header parsing exists.
- **Session store**: In-memory session store with configurable TTL and pluggable backend interface. HMAC-signed session cookie for integrity. Both Drogon and Crow include this built-in; it is the most basic state management primitive for web applications.

#### Response caching middleware (`AERONET_ENABLE_RESPONSE_CACHE`)

In-memory LRU cache keyed by method + path + `Vary` headers. Respects `Cache-Control` directives (`max-age`, `no-store`, `no-cache`, `private`). Configurable max entries and memory budget. Per-route opt-in via middleware registration. ETag / `If-None-Match` validation for cached entries. Useful for APIs with expensive computation behind cacheable endpoints.

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
the [JWT guide](modules/jwt.md) and `aeronet/jwt/test/`.

Still planned: a server-side **middleware** that parses the `Authorization` header (Basic credentials or
Bearer tokens) and wires the JWT verifier into a pluggable validator interface; plus a client-side
**JWKS-fetch + cache** helper building on the HTTP client.

#### Reverse proxy / HTTP forwarding mode

Forward incoming requests to upstream backends, rewriting headers (`X-Forwarded-For`, `X-Forwarded-Proto`, `Via`). Builds naturally on the delivered HTTP client module. Load balancing strategies (round-robin, least-connections). This turns aeronet from a pure application server into an edge/gateway server - a common deployment pattern (Nginx, Caddy, Envoy, Traefik, HAProxy).

#### Inbound request body streaming to handler

Deliver chunked request bodies to handlers as they arrive instead of buffering the full payload first. Enables processing uploads in bounded memory. Currently documented as a limitation in the [requests guide](guides/requests.md#limits-and-timeouts). Express (streams), Go (`io.Reader`), Axum (`BodyStream`), Actix-web (`Payload`), and Rust Hyper all expose streaming request bodies.

#### Streaming multipart parsing

Process multipart/form-data uploads part-by-part as data arrives, rather than buffering the entire payload. Critical for large file uploads where the full body does not fit in memory. Currently documented as a limitation in the [requests guide](guides/requests.md#forms-and-file-uploads). Popular in Multer (Node.js), Actix-multipart, Spring `StreamingMultipartResolver`, and Go `multipart.Reader`.

#### Per-route body size limits

Status: **Delivered in `1.3.0`** - per-route `maxBodyBytes` / `maxHeaderBytes` overrides on `PathHandlerEntry`
and `RouteGroup`, enforced across HTTP/1.1 (sync + async) and HTTP/2 with `413` / `431`.

#### Graceful config reload via signal (SIGHUP)

Reload the JSON/YAML config file on `SIGHUP` without restarting the server. Apply mutable config changes (timeouts, limits, compression settings, TLS certs) hot. Pairs with the existing Glaze config loader and `postConfigUpdate()`. Nginx, HAProxy, Caddy, and systemd-based services all support `SIGHUP`-driven reload.

#### WebSocket broadcast / pub-sub helpers

Built-in topic-based broadcast for WebSocket connections. `WsRoom` / `WsTopic` abstraction where connections subscribe to topics and the server broadcasts to all subscribers. Common pattern in Socket.IO, uWebSockets, Phoenix Channels, and SignalR. Reduces boilerplate for chat, dashboards, and real-time notification use cases.

#### ETag support for dynamic responses

Compute a weak or strong ETag from the response body (e.g. hash) and handle `If-None-Match` / `304 Not Modified` for dynamic endpoints, not just static files. Reduces bandwidth for API responses that rarely change. Express (`etag` by default), Rails, ASP.NET, and Spring all auto-generate ETags for regular responses.

### TLS enhancements (future)

- Per-SNI mTLS policies
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
