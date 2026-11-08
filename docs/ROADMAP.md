# aeronet Roadmap - Planned / Not Implemented

## High priority

- **Security Hardening Audit**:
  - TLS fingerprinting hardening (avoid leaking version/cipher info in edge cases)
  - Fuzzing harness integration (libFuzzer + AFL), first for the HTTP/1.1 and HTTP/2 parsers

## Medium priority

- **Pluggable logging sink API (non-access logs)** - spdlog backend supports custom sinks/formatters; an aeronet-native sink registration API is not yet exposed.
- **Enhanced parser diagnostics** (byte offset in parse errors for better debugging)
- **Manage TE header in the client**: currently the TE header is reserved in the framework, so it cannot be sent. It could be a good idea to have a flag in the config to allow sending TE headers in the client with trailers essentially (because trailers are natively supported by aeronet). If `TE: trailers` is sent, in HTTP/1.1 we also need to add `Connection: TE`.
- **Protocol Buffers, next steps** (after the [delivered](modules/protobuf.md) bodies and `Accept` negotiation):
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
extend them instead of duplicating them.

| Benchmark gap | Decision it must support |
| --- | --- |
| Stream randomized lookup, frame processing and prune churn through 10 K streams | Stream map/cache and hot/cold split |
| Response-size syscall counts, plain + TLS | TCP cork threshold |
| Header lookup/mutation with 4 through 32 fields | Flat scan versus compact/lazy indexing |
| JWT decode/verify with 1 through 1,000 JWKs | `kid` index and base64url decode strategy |
| Codec alternating-size sessions with allocation and peak-capacity counters | Scratch growth/shrink and context reuse |
| Fragmented + compressed WebSocket message processing | End-to-end frame/reassembly path beyond mask-only throughput |
| Hot-structure `sizeof` and 10 K-object random-access profile | Layout changes without speculative padding claims |

## Long-term / Nice-to-have

- **HTTP/3 / QUIC** - a separate transport layer (future major feature); scoped as a research spike below before any commitment.
- **Per-SNI mTLS Policies** - Different client cert requirements per SNI hostname
- **Per-route metrics** - request counts and latency per route

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

Delivered, see the [HTTP client guide](modules/http-client.md). Still planned (receive sizing and DNS/TLS reuse are in
the [performance optimization backlog](#performance-optimization-backlog)):

- **Coroutine-friendly API** (`co_await client.get(...)`) integrated with a *running* server event loop, so handlers can issue upstream calls without blocking. This is also what unlocks **true HTTP/2 multiplexing** in the client: the native HTTP/2 engine has landed, but the synchronous model runs one stream at a time per connection.
- **Unify the request buffer with the wire bytes**: teach `HttpMessage` a request-line first-line mode so the client builds the request directly in the buffer it writes, with zero reassembly (today request fields are copied once into the send buffer).
- **Cancellation from another thread** of an in-flight client exchange (a `ResponseBodySink` can already stop a transfer, and `inactivityTimeout` bounds a stalled one).
- **Client-side telemetry**: surface pool hit/miss, retry counts, cache hits/misses, redirect hops and decompression stats through the same `TelemetryContext` infrastructure the server already uses (the client currently emits none).
- **Cookies, next steps** (after the [delivered](modules/http-client.md#cookies) in-memory jar):
  - import and export of the Netscape `cookies.txt` format (curl, wget, yt-dlp, browser extensions), so that a long-running client keeps its session across restarts and can start from the cookies of a browser;
  - the public suffix list, behind the heuristic of `RegistrableDomain()` (`aeronet/client/src/cookie-jar.cpp`): an optional libpsl (vcpkg, Conan, Linux systems) for exact registrable domains (`github.io`), and same-site checks on registrable domains instead of hosts.
- **Reverse-proxy / forwarding** building on the client (see the reverse-proxy item below).

#### HTML parsing and JavaScript pages (`AERONET_ENABLE_HTML`, `AERONET_ENABLE_JS`)

Status: [HTML parsing](modules/html.md) (phase 1) and the [client cookies](modules/http-client.md#cookies) (phase 2)
delivered, JavaScript not started
([#738](https://github.com/sjanel/aeronet/issues/738)). Goal: let the client read pages whose content is produced or
completed by JavaScript, e.g. `page.querySelectorAll("table tr")` after a `navigate()`.

**The JS engine is the small part.** Embedding one is a few C files; the DOM, the script loading rules, the event
loop and the Web APIs (`fetch`, XHR, timers, `location`, storage, observers...) are most of the work, and that
surface is never complete: Lightpanda, a funded headless browser built on V8, still describes its Web API coverage
as partial. On the other hand many "JavaScript pages" do not need JS at all: they are rendered on the server, or
they embed their data as JSON that the page scripts turn into markup, which phase 1 reads (the
[Kraken withdrawal fees page](https://support.kraken.com/articles/360000767986-cryptocurrency-withdrawal-fees-and-minimums)
of #738 has no `<table>`: its 799 rows are in `window.__INITIAL_PROPS__`, behind 51 scripts).

Worked example, a [Nintendo Store product page](https://store.nintendo.com/fr-fr/manette-pro-nintendo-switch2-edition-40e-anniversaire-de-the-legend-of-zelda-support-dexposition-000000000010019437)
watched for its stock (steps 1-3 checked on 2026-11-07, step 4 from a headless Chromium dump of 2026-11-02). Its
data needs no JS, but reaching it does: the store is behind a Queue-it waiting room that sends every new visitor
through a script.

1. A request without cookies is answered `307` to `nintendostoreuk.queue-it.net`.
2. That page (2.4 KB) has one inline script: it checks `navigator.cookieEnabled` and `document.cookie`, then sets
   `document.location.href` to a relative URL carrying a signed timestamp. Its `<noscript>` says "The site requires
   JavaScript".
3. When no queue is running, that URL answers `302` back to the product page, with a signed `queueittoken` in the
   query and in a `Queue-it-token` cookie (3 minutes, `SameSite=None`).
4. The product page (1.25 MB, Next.js) holds the stock state in `<script id="__NEXT_DATA__" type="application/json">`
   (1 MB): `props.pageProps.product.inventory.orderable`, `c_availabilityModel.type` (`"OutOfStock"`), the price.
   Its ~25 scripts (Next.js chunks, consent, account SDK, analytics) are not needed. Searching the HTML for
   "rupture de stock" finds it whatever the stock: the words are in the translations of `__NEXT_DATA__`.

What it needs, in the phases below: a cookie jar across sites (phase 2, delivered: steps 1 to 3 already keep and send
the cookies of both sites), script navigation, parsing in scripting mode, and a selector checked before the scripts
of each document run (phase 3). When a queue is running, step 3 is
a real waiting room (it polls Queue-it, and may ask for a CAPTCHA or a proof of work): out of scope like the other
anti-bot challenges, the client does not try to skip it, `navigate()` ends at its budget on the queue-it.net
document.

Dependencies (C libraries, kept out of the public headers):

- **lexbor** (Apache-2.0, already used by phase 1): its per-element mutation callbacks (insertion, removal,
  attribute change) are the hook for scripts inserted at runtime and `MutationObserver`; each node has a `user`
  pointer to cache its JS wrapper (the one of the document node holds the query engine of `HtmlDocument`); a
  document can be parsed in scripting mode (`lxb_html_document_scripting_set`).
- **QuickJS-ng** (MIT, ES2023+, CMake, MSVC, vcpkg): not in Conan Center, which only has Bellard's QuickJS -
  either contribute a recipe or support both behind a thin layer (their C APIs are close). No `Intl` (the
  project says it will likely never have it, so i18n libraries need stubs), no WebAssembly, interpreter speed.
  V8, SpiderMonkey and JavaScriptCore are rejected for binary size and build complexity; Duktape, MuJS and
  JerryScript cannot parse modern bundles (ES5).

Phases:

1. **HTML** - delivered. Still open: DOM mutation and serialization of a modified tree (needed by phase 3 anyway).
   The fragment parsing behind `innerHTML` and `insertAdjacentHTML` must get the same nesting limits as the document
   parsing (`aeronet/html/src/html-parse.cpp`).
2. **Client cookie jar** - delivered, see [Cookies](modules/http-client.md#cookies): RFC 6265bis storage and
   retrieval, `SameSite` along redirect chains across sites, merged with the `Cookie` header of the request.
3. **JS core** (`AERONET_ENABLE_JS`, requires `AERONET_ENABLE_HTML` and `AERONET_ENABLE_HTTP_CLIENT`, OFF by
   default), module `aeronet/web`: `WebClient::navigate(url, options)` returning a `std::expected`, the same
   `Element` API as phase 1, and `evaluate()`.
   - One QuickJS runtime per page over the lexbor document; the page owns every node and frees them together,
     so node lifetime does not depend on the JS garbage collector.
   - Documents are parsed in scripting mode, as browsers with JS do: `<noscript>` content is text, not markup.
   - Scripts run in document order with `defer` semantics after a full parse (`document.write` during parsing
     is limited).
   - Script navigation (`location.href`, `location.assign()` / `replace()`, `<meta http-equiv="refresh">`) drops
     the page and its runtime and loads the new URL, resolved against the document URL; the hops count toward a
     limit, like redirects.
   - Event loop: one task, then the microtasks (`JS_ExecutePendingJob`). Timers use a virtual clock: they fire
     in order without sleeping, with caps on far timers and on `setInterval` runs.
   - `fetch` and XHR go through the page's `HttpClient` (synchronous XHR comes for free). `navigator` has
     `cookieEnabled`, `userAgent` and `language`. `document.cookie` reads and writes the jar of the client as a
     non-HTTP API (`HttpOnly` cookies hidden and kept, RFC 6265bis §5.7), and the subresources of a page are
     requests of its site: their `SameSite` context comes from the page, not from a redirect chain.
   - `navigate()` returns on network idle (no request in flight, no timer due), on a matching selector, or when
     the time budget runs out (QuickJS interrupt handler). The selector is checked when a document is parsed,
     before its scripts run: data served in the HTML costs no script run.
4. **Single-page app compatibility**: ES modules and `import()`, scripts inserted at runtime (webpack chunk
   loading) through the lexbor mutation callbacks, `MutationObserver`, stubs (`Intl`, `matchMedia`,
   `getComputedStyle`, `IntersectionObserver` reporting everything visible, layout sizes of 0),
   `waitForSelector()`, then clicks and form input.
5. **Optional, independent**: a driver for full-fidelity browsers (Chrome, Lightpanda) over CDP (JSON over
   WebSocket). Little aeronet code, but it needs a WebSocket client, which aeronet does not have.

```cpp
aeronet::WebClient web;  // its cookie jar outlives navigate()

// A single-page app: run the scripts until the network is idle.
auto page = web.navigate(url, {.until = aeronet::LoadState::NetworkIdle, .timeout = std::chrono::seconds{10}});
if (page) {
  auto rows = page->querySelectorAll("table tr");
}

// The Nintendo Store page above: 4 exchanges and the waiting-room script, none of the product page scripts.
auto product = web.navigate(nintendoUrl, {.untilSelector = "script#__NEXT_DATA__", .timeout = std::chrono::seconds{10}});
if (product) {
  std::string nextData = product->querySelector("script#__NEXT_DATA__").textContent();  // parse with glaze
}
```

Scope: server-rendered pages with JS on top, and single-page apps (React, Vue, Svelte, webpack) that only need the
DOM, `fetch` / XHR and timers. Out of scope: rendering and layout, anti-bot challenges (they fingerprint canvas,
WebGL and timing), WebAssembly, workers, iframes, canvas, media.

Performance: the synchronous client fetches the subresources one at a time (51 sequential exchanges for the Kraken
page); the coroutine client with HTTP/2 multiplexing (above) is the fix. Measure the QuickJS-ng compile and run
time of real bundles before committing to phase 3.

Security: `navigate()` runs remote code in the host process, and QuickJS-ng had memory-safety CVEs in 2025-2026
(typed arrays, `Atomics`, iterator helpers). Never link `quickjs-libc` (no file or OS access); set memory and stack
limits and a CPU budget; add a network policy (allowed origins, request and byte caps); leave out
`SharedArrayBuffer` and `Atomics`; pin the engine version, fuzz the bindings and run the JS tests in the ASAN CI
legs. Document that untrusted sites belong in an unprivileged process; an out-of-process sandbox (seccomp,
landlock) can come later.

Tests: pages served by aeronet's own `HttpServer` (a cookie check and script redirect across two origins, a
webpack-style chunk loader, a vendored production React build rendering a table), so CI needs no internet.

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

Parse `Accept` header q-values to select response content type (JSON, YAML, XML, plain text). Return `406 Not Acceptable` when no format matches. Apart from `Accept-Encoding`, and from `makeProtoResponse()` choosing between binary protobuf and ProtoJSON, `Accept` (media type) is left to user code. Frameworks like Spring, Rails, Phoenix, and ASP.NET handle this transparently.

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

[JWT verification](modules/jwt.md) is delivered. Still planned: a server-side **middleware** that parses the
`Authorization` header (Basic credentials or Bearer tokens) and wires the JWT verifier into a pluggable validator
interface; plus a client-side **JWKS-fetch + cache** helper building on the HTTP client.

#### Reverse proxy / HTTP forwarding mode

Forward incoming requests to upstream backends, rewriting headers (`X-Forwarded-For`, `X-Forwarded-Proto`, `Via`). Builds naturally on the delivered HTTP client module. Load balancing strategies (round-robin, least-connections). This turns aeronet from a pure application server into an edge/gateway server - a common deployment pattern (Nginx, Caddy, Envoy, Traefik, HAProxy).

#### Inbound request body streaming to handler

Deliver chunked request bodies to handlers as they arrive instead of buffering the full payload first. Enables processing uploads in bounded memory. Currently documented as a limitation in the [requests guide](guides/requests.md#limits-and-timeouts). Express (streams), Go (`io.Reader`), Axum (`BodyStream`), Actix-web (`Payload`), and Rust Hyper all expose streaming request bodies.

#### Streaming multipart parsing

Process multipart/form-data uploads part-by-part as data arrives, rather than buffering the entire payload. Critical for large file uploads where the full body does not fit in memory. Currently documented as a limitation in the [requests guide](guides/requests.md#forms-and-file-uploads). Popular in Multer (Node.js), Actix-multipart, Spring `StreamingMultipartResolver`, and Go `multipart.Reader`.

#### Graceful config reload via signal (SIGHUP)

Reload the JSON/YAML config file on `SIGHUP` without restarting the server. Apply mutable config changes (timeouts, limits, compression settings, TLS certs) hot. Pairs with the existing Glaze config loader and `postConfigUpdate()`. Nginx, HAProxy, Caddy, and systemd-based services all support `SIGHUP`-driven reload.

#### WebSocket broadcast / pub-sub helpers

Built-in topic-based broadcast for WebSocket connections. `WsRoom` / `WsTopic` abstraction where connections subscribe to topics and the server broadcasts to all subscribers. Common pattern in Socket.IO, uWebSockets, Phoenix Channels, and SignalR. Reduces boilerplate for chat, dashboards, and real-time notification use cases.

#### ETag support for dynamic responses

Compute a weak or strong ETag from the response body (e.g. hash) and handle `If-None-Match` / `304 Not Modified` for dynamic endpoints, not just static files. Reduces bandwidth for API responses that rarely change. Express (`etag` by default), Rails, ASP.NET, and Spring all auto-generate ETags for regular responses.

## Realistic Network Testing

Goals

- Improve confidence that aeronet behaves correctly under latency, jitter, packet loss, reordering, partial delivery, and connection resets.
- Catch protocol-level bugs (HTTP/1.1 and HTTP/2), TLS handshake fragility, flow-control and resource-leak issues that do not appear on a perfect local loopback.

Approach and Components (beyond the existing `TestTransport` / `FaultInjectingTransport` tests)

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

CI Policy: deterministic and fault-injection tests run on PRs; proxy-based and `tc netem` suites run nightly, non-blocking for PRs.

Milestones

1. Add HTTP/2 direct protocol tests using `TestTransport` + `processInput()` for frame-level fault injection.
2. Add simple Toxiproxy-based harness and a handful of end-to-end tests (connection cut, high latency, truncated responses).
3. Add `ip netns` + `tc netem` scripts in `tests/e2e/` and integrate nightly CI job.
4. Collect failure artifacts (pcap + logs) and add tooling to reproduce failing scenarios locally with the same netem parameters.

Known Limitations

- **Read-EAGAIN incompatible with EPOLLET in integration tests**: When `FaultInjectingTransport` returns `{0, ReadReady}` (simulated EAGAIN), the server breaks to wait for a new EPOLLIN edge. But with EPOLLET, the socket already has data so no edge fires - causing a hang. Workaround: EAGAIN on reads is tested only at unit level; integration tests use partial reads (which work because `EPOLL_CTL_ADD` with existing data triggers an initial event).

Acceptance Criteria

- Protocol correctness under simulated faults: no state corruption, proper error propagation, and graceful teardown.
- No resource leaks (sockets, memory) in faulted runs.
- Deterministic unit tests reproduce failures locally with a seed and do not rely on privileged resources.

## Test suite reduction and speed

Status: a first pass was done on 2026-11-08 (6024 -> 5888 tests, about 1,600 lines less, full `ctest -j12` from 11.4 s to 9.3 s, no covered line lost). Nothing below is implemented yet. Numbers are from that date, in a Debug build under `ctest -j12`. The slowest binaries are then `http-core_test` (9.2 s), `http-client-core_test` (7.3 s) and `http-client-tls-e2e_test` (5.6 s).

Goals

- Every test binary well under 10 s in Debug.
- Each behaviour tested once, at the layer where it is cheapest, plus one integration scenario proving the wiring.
- No loss of coverage, nor of asserted behaviour.

Rules for a reduction pass

- Prefer merging into a table over deleting: tests going through the same code often assert different values (parsers, formatters, validators). Delete only literal repeats and tests whose assertions are a subset of another test.
- Compare line and branch coverage per file before and after (lcov export of the coverage build): totals can hide a line lost here and gained there. The usual loss is an rvalue overload, when a merge keeps only the lvalue call.
- A few branches differing between two runs is noise (timing-dependent paths, `compare_exchange_weak`): run twice before investigating.
- The docs link to tests by name: search `docs/` for each removed name.
- Merged tests on a shared server keep its config and route leaks: run the whole binary, not only the merged test.
- Do not shorten the timings of tests tuned for loaded CI runners without a deterministic replacement.

### Finding candidates

- **Near-duplicate bodies**: extract every test body, replace the literals by placeholders and cluster by Jaccard similarity of 4-token shingles (threshold 0.8). Result: 398 clusters, 1336 tests, 16,373 lines.
- **Per-test edge coverage**: a throwaway tree built with `-O0 -g1 -fsanitize-coverage=edge,inline-bool-flag,pc-table` and a gtest listener linked in each test executable, resetting the flags in `OnTestStart` and dumping them in `OnTestEnd`, gives the library edges reached by each test (about 5 minutes in RAM). Edges are needed because source-based coverage regions can be differences of counters, which makes per-test subsets wrong. Pitfalls: the allowlist needs `fun:*` on top of its `src:` lines; `-z defs` must be dropped so that the shared libraries can reference the callbacks defined by the executable; a module is identified with `dladdr` on its flags, not on its first PC (an interposed comdat function points to another module); only the edges of library sources count (symbolize the function entries).
- 3904 of 6023 tests had no edge of their own at the first measurement. That only ranks candidates: combine it with the clusters and read the assertions.

### Remaining duplicated tests

| Test file | Lines in clusters | What to merge |
| --- | --- | --- |
| `http2-protocol-handler_test.cpp` | 1633 | `StreamingHandler*` single-request variants, file payloads (`LargeFilePayloadFlowControlledSending`, `FilePayloadExactlyFillsInitialWindow`), CORS (`*WithCorsApplied`, `*MiddlewareShortCircuitWithCors`, `CorsPreflight*`), `Options*` / `TraceReturns405InHttp2`, `PerRouteMaxHeaderBytes*` and `PerRouteMaxBodyBytes*` pairs, `RequestTrailers*Rejected`, `*Http2Disable*Returns404`. Keep the `AsyncHandler*DeferWork*` tests, which have many unique edges. |
| `tests/http-core_test.cpp` | 664 | `HttpResponseWriterFailures` (`FileAfterWriteBody`, `WriteBodyWithFileActive`, `ContentLength*`, `ModifyAfterHeadersSent`), `HttpStreaming.SendFile*`, `HttpCorsDetailed.Preflight*`, `HttpCorsIntegration.Streaming*`, `HttpStreamingHeadContentLength.*`, `HttpHeaderTimeout.Emits408*`, `HttpRateLimit.*` pairs. |
| `router_test.cpp` | 576 | `CatchAll*`, `Overwrite*`, `AllowedMethods*`, `ConstraintUnconstrainedFallback*`, patterns that must throw (`*Throws`, `*Disallowed`). |
| `http-client-core_test.cpp` | 537 | `FollowsRedirect`, `FollowsMovedPermanently`, `FollowsTemporaryRedirect`, `FollowsPermanentRedirect`, `StatusRetry*`, `HttpClientTrailerE2ETest.*`. |
| `upgrade-handler_test.cpp` | 523 | `ValidateWebSocketUpgrade_*`: a table for extensions and deflate offers, one for subprotocols, one for rejected requests. |
| `hpack_test.cpp` | 505 | The 21 decoding scenarios around invalid fields (`Rejects*`, `DuplicateIndexedHeaderForbidden`...), `EncodeNeverIndexed_*` / `EncodeWithoutIndexing_*`. |
| `http2-connection_test.cpp` | 407 | The frame validation family (`*IsFrameSizeError`, `*IsProtocolError`, `*IsFlowControlError`): a table of frame bytes and expected error. `PriorityChain*`. |
| `http-request-view_test.cpp` | 403 | `MergeConsecutiveHeaders*` / `MergeNonConsecutiveHeaders*`, the requests rejected on parsing, `MakeResponse*`. |
| `static-file-handler_test.cpp` | 390 | `MultiRange*` (17 tests), `Conditional*`. |
| `multipart-form-data_test.cpp` | 371 | The 24 rejected or accepted bodies: a table of body and expected outcome. |
| `server-config-loader_test.cpp` | 362 | `RouterCorsPolicy*`, `*SaveConfig{Yaml,Json,Yml}`, `Duration*RoundTrip`, field mapping tests with identical edges. |
| `tls-ticket-key-store_test.cpp` | 345 | `ProcessTicketFailsWhen*`, `LoadStaticKeys*`. |
| `route-constraint_test.cpp` | 341 | The 33 regex and quantifier cases, the 9 malformed patterns. |
| `tests/http-compression_test.cpp` | 335 | `DirectCompression_*`, negotiation results (`GzipChosenWhenHigherPreference`, `QValuesAffectSelection`...). |
| `websocket-handler_test.cpp` | 310 | `Utf8*`, reserved opcodes, `*MessageTooLarge`, masked pings. |
| `http-request_test.cpp` (client) | 309 | `ResolveRedirect*`: a table of base, location and result. |
| `http-response_test.cpp` | 309 | `SendFilePayloadOffsetLength*`, `SendFileHead*`, `Body_OnMode_Bypasses*`, `Body_BelowMinBytes_NoDirectCompression` / `Body_OffMode_NoCompression`. |
| `websocket-frame_test.cpp` | 307 | `Parse*` rejections, `HeaderSize*`. |
| `http1-response-parser_test.cpp` | 300 | Rejected responses, `Connection` token lists. |
| `tests/http2-core_test.cpp` | 294 | The 13 `Http2Streaming.*` single-handler tests. |
| `tests/http-routing_test.cpp` | 279 | `AsyncHandler*WithBodyNotReady`, `AsyncReadBodyBeforeBody*`. |
| `tests/http-additional_test.cpp` | 275 | `HandlerCanEmit*`, `EpollCtlMod*`, `EpollRdhup*` / `EpollHup*`. |
| `tests/http-errors_test.cpp` | 263 | Tables already, whose drivers can be shared. |
| `timestring_test.cpp` | 244 | Rejected dates and the formatting cases (identical edges, the values matter). |
| `websocket-deflate_test.cpp` | 227 | `ParseDeflateOffer_*`, `BuildDeflateResponse_*`. |

Other kinds of duplication:

- **Across files** (19 clusters, 766 lines):
  - `brotli-`, `zstd-` and `zlib-encoder-decoder_test.cpp` repeat the same scenarios per codec: typed tests.
  - `http-client-core_test.cpp` and `http-client-http2-e2e_test.cpp`: scenarios parameterized on the protocol. `http-client-integration_test.cpp` also repeats basic ones.
  - Header key and value validation in `http-client-config_test.cpp` and `http-server-config_test.cpp`.
  - "Default config is valid" tests, repeated per config object.
  - TLS version parsing in `tls-config_test.cpp` and `server-config-loader_test.cpp`.
- **Across layers**: behaviours tested in full in both a unit and an integration test can keep one integration scenario: Range and conditional requests (`HttpRangeStatic`, `HttpRangeMulti` and `static-file-handler_test.cpp`), CORS (`HttpCorsDetailed`, `cors-policy_test.cpp` and the HTTP/2 handler tests), HTTPS redirect, request parsing errors.
- **HTTP/1.1 and HTTP/2 pairs**: behaviours tested separately in `http-core_test` and `http2-core_test` can move to the scenarios of `tests/http-protocol-parity_test.cpp`, which run on both protocols.
- **Identical edges, values that matter**: `websocket-compress_test.cpp` (groups of 13 and 9 tests), `tchars_test.cpp`, `base64_test.cpp`, `mergeable-headers_test.cpp`, `file-sendfile_test.cpp`: tables, without dropping cases.

### Repeated boilerplate

- `http2-protocol-handler_test.cpp`: 168 `Http2ProtocolLoopback` setups, 222 `pumpClientToServer()` calls, 38 identical default routes and 22 hand-built pseudo-header blocks. A helper that sends a list of pseudo-headers and pumps both ways would shorten most tests.
- `RequestOptions` + `requestOrThrow` + `parseResponseOrThrow` (74 uses in `http-core_test`, 20 in `http-routing_test`): one helper returning the parsed response.
- `ClientConnection` + `sendAll` + `recvUntilClosed` (86 connections in `http-core_test`, 77 in `http-additional_test`, 33 in `http-errors_test`): `test::sendAndCollect` when the socket is not used afterwards. The first pass only rewrote the 30 exact matches.
- `StaticFileHandlerTest`: the sequence of `buildReqWithHeaders`, `setHead` and handler call, in each test.
- One convention for table tests: `SCOPED_TRACE` loops are lighter, `TEST_P` (used in 7 files only) can filter and report each case.

### Slow tests

Dead waits, safe to remove:

- `test::recvWithTimeout` does not stop at an EOF received before a complete head: it spins until its timeout. It should return on `recv() == 0`.
- 81 calls use its default 500 ms timeout without an expected size (20 in `http-core_test`, 18 in `http-errors_test`, 11 in `websocket-integration_test`): each one not reaching a complete response waits the whole timeout.
- `recvWithTimeout(fd, 1000ms)` before a `WaitForPeerClose`, whatever is received: `HttpResponseDispatchErrors.QueueDataTransportError`, `HttpResponseDispatchErrors.FlushOutboundTransportError`, `Http1WriterTransport.WriteBodyReturnsFalseAfterTransportError` (1 s each). `RecvUntilEnd` gives the data and the end in one call. Same for `HttpTlsNegative.PlainHttpToTlsPortRejected` (0.5 s).
- Private servers on the default `pollInterval` of 500 ms, whose stop waits for a whole poll: six tests of `http-server-lifecycle_test` sit at exactly 0.5 s, `HttpServerMove.MoveAfterCompressingKeepsCompressing` (three servers) at 1.5 s, and probably `HttpTlsMoveAlpn.*`. Use `withPollInterval` unless the poll interval is what is tested.
- Plateaus not explained yet: 0.25 s (`HttpRouting.AsyncHandlerThrowsWithBodyNotReady`, `HttpRouting.AsyncHandlerNonStdExceptionWithBodyNotReady`, `HttpClientTlsE2ETest.GetOverTlsWithoutVerification`, `HttpClientTlsErrorTest.PartialClientCertificateFileConfigurationIsIgnored`, `HttpClientMtlsTest.RejectsClientWithoutCert`, `Http2ConnectTest.WildcardAllowlistAllowsTarget`) and 0.5 s (`Http2ConnectTest.DnsFailureReturns502`).

Waits for a negative outcome:

- `TunnelResolverTest.CancelDropsQueuedResolutions` (1.5 s) waits its whole 1500 ms bound for resolutions that must never complete: wait for the last resolution in progress instead. To check as well: `TunnelResolverTest.DestructionDoesNotWaitForResolutionsInProgress` (1.25 s).

Waits for real time, needing an injectable clock or a sub-second interval:

- `HttpTlsHandshakeCallback.RefillsRateLimitAfterInterval` (1.8 s, whole-second token bucket).
- `TlsTicketKeyStoreTest.RotateExceedsMaxKeysPopsBack` (1.1 s) and `HttpTlsSessionTickets.AutoRotationRefreshesPrimaryKeyAndRejectsUnknown` (1 s), rotation intervals in whole seconds.
- `HttpDate.ChangesAcrossSecondBoundary` (up to 1 s).

Real work, to reduce:

- `TlsContextTest.EveryAllocationFailureDuringConstructionThrowsCleanly` (4.1 s, the slowest test): one context construction per allocation, with a smaller certificate set for example.
- `HttpCodecCompression.TryCompressResponseStressWithDifferentScenarios` (1.7 s): seven nested dimensions, pairwise combinations would do.
- `WebSocketUtf8.EveryLeadWithBoundaryContinuations`, `RandomMutations` and `EveryBytePair` (2.9 s together): exhaustive, a smaller sample in Debug.
- Payload sizes of `ProtocolParityFastBrotli.LargeIncompressibleBodiesAreCompressedWhole` (0.8 s twice), `HttpClientSinkE2ETest.ContentCodedBodiesAreDecodedAsTheyArrive` (0.7 s) and `HttpClientCompressionE2E.LargeIncompressibleDirectlyCompressedRequestBody` (0.6 s).

Tuned to real timing, to leave alone without deterministic hooks: `ZerocopyMode.*`, `HttpBackpressure.*`, `HttpKeepAlive.*` (most of the remaining time of `http-core_test`), `HttpConnectDefaultConfig.SlowResolutionDoesNotBlockTheEventLoop`, `WebSocketTimeouts.PongsKeepIdleConnectionOpen`, `HttpClientErrorE2ETest.InactivityTimeoutLetsASteadySlowResponseComplete`, `Http2Core.PingKeepAliveClosesTheConnectionOfADeadPeer`.

### Test infrastructure

- **Commit the tools**: the near-duplicate clustering and the per-test edge coverage, in `scripts/`, so that each pass does not have to rebuild them.
- **Duration budget in CI**: from the gtest JSON output (`GTEST_OUTPUT`), warn when a binary exceeds 10 s in Debug.
- **Test isolation**: the binaries sharing one server leak config and routes between tests (71 config updates on the shared server of `http-core_test` alone), so `--gtest_shuffle` and `--gtest_repeat` fail spuriously. `ScopedConfigUpdate`, or a reset of config and routes in a fixture `SetUp`, would remove the dependency on test order.
- **Sharding**: once isolated, the largest binaries can run in several processes (`GTEST_TOTAL_SHARDS`): the longest binary sets the wall time under `ctest -j`.
- **Split the largest files by theme**: `http2-protocol-handler_test.cpp` (7047 lines), `tests/http-core_test.cpp` (6559), `http-response_test.cpp` (5914), `http-client-core_test.cpp` (5221), `http2-connection_test.cpp` (4521). Each file is one translation unit recompiled at each edit.
