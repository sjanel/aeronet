# Aeronet

![Aeronet Logo](resources/logo.png)

HTTP/1.1 C++ server library for Linux only – work in progress.

## Core HTTP & Protocol Features (Implemented)

| Feature | Notes |
|---------|-------|
| HTTP/1.1 request parsing | Request line + headers + `Content-Length` bodies (minimal allocations) |
| Chunked request decoding | `Transfer-Encoding: chunked` (trailers parsed but not exposed yet) |
| Response building | Convenience struct & helpers (status + headers + body) |
| Keep-Alive | Timeout + max-requests per connection; HTTP/1.0 opt-in |
| Percent-decoding | UTF-8 path decoding, invalid sequences -> 400 |
| Pipelining | Sequential (no parallel handler execution) |
| Configurable limits | Max header bytes, max body bytes, max outbound buffer bytes |
| Date header caching | 1 update / second (RFC7231 format) |
| HEAD method | Suppresses body while preserving `Content-Length` |
| Expect: 100-continue | Sent only when request has a (non-zero) body |
| Per-path routing | Exact path match with method allow‑lists |
| Trailing slash policy | Strict / Normalize (default) / Redirect |
| Mixed-mode dispatch | Deterministic precedence: path streaming > path normal > global streaming > global normal |
| Streaming responses | Chunked by default; switch to fixed length with `setContentLength()` |
| Slowloris mitigation | Header read timeout (configurable; disabled by default) |
| 404 / 405 handling | Automatic 404 (unknown path) & 405 (known path, method not allowed) |
| Graceful shutdown | `runUntil()` predicate loop (poll interval via `HttpServerConfig::withPollInterval()`) |
| Backpressure buffering | Unified buffering for fixed + streaming responses |
| Response compression (gzip & deflate) | Optional (AERONET_ENABLE_ZLIB). Accept-Encoding negotiation with q-values, server preference ordering, threshold-based activation, streaming + buffered, per-response opt-out, Vary header injection |

## Developer / Operational Features

| Feature | Notes |
|---------|-------|
| Epoll edge-triggered loop | One thread per `HttpServer`; writev used for header+body scatter-gather |
| SO_REUSEPORT scaling | Horizontal multi-reactor capability |
| Multi-instance wrapper | `MultiHttpServer` orchestrates N reactors, aggregates stats (explicit `reusePort=true` required for >1 threads; port resolved at construction) |
| Async single-server wrapper | `AsyncHttpServer` runs one server in a background thread |
| Move semantics | Transfer listening socket & loop state safely |
| Heterogeneous lookups | Path handler map accepts `std::string`, `std::string_view`, `const char*` |
| Outbound stats | Bytes queued, immediate vs flush writes, high-water marks |
| Lightweight logging | Pluggable design (spdlog optional); ISO 8601 UTC timestamps |
| Builder-style config | Fluent `HttpServerConfig` setters (`withPort()`, etc.) |
| Metrics callback (alpha) | Per-request timing & size scaffold hook |
| RAII construction | Fully listening after constructor (ephemeral port resolved immediately) |
| Comprehensive tests | Parsing, limits, streaming, mixed precedence, reuseport, move semantics, keep-alive |
| Mixed handlers example | Normal + streaming coexistence on same path (e.g. GET streaming, POST fixed) |

The sections below provide a more granular feature matrix and usage examples.

## Quick Start (Minimal Server)

Spin up a basic HTTP/1.1 server that responds on `/hello` in just a few lines. If you pass `0` as the port (or omit it), the kernel picks an ephemeral port which you can query immediately.

```cpp
#include <aeronet/aeronet.hpp>
#include <print>
using namespace aeronet;

int main() {
  HttpServer server(HttpServerConfig{}.withPort(0)); // 0 => ephemeral port
  server.addPathHandler("/hello", http::MethodSet{http::Method::GET}, [](const HttpRequest&) {
    return HttpResponse(200, "OK").contentType("text/plain").body("hello from aeronet"\n);
  });
  std::print("Listening on {}\n", server.port());
  // Adjust event loop poll interval (max idle epoll wait) if desired (default 500ms)
  // HttpServerConfig{}.withPollInterval(50ms) for more responsive stop/predicate checks.
  server.run(); // Blocking call, send Ctrl+C to stop
}
```

Build & run (example):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/examples/aeronet-minimal 8080   # or omit 8080 for ephemeral
```

Test with curl:

```bash
curl -i http://localhost:8080/hello
```

Example output:

```text
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 18
Connection: keep-alive

hello from aeronet
```

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
- [x] Mixed-mode dispatch (simultaneous registration of streaming and fixed handlers with precedence)
- [x] Compression (gzip & deflate) (phase 1: zlib) – streaming + buffered with threshold & q-values

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
- [x] Percent-decoding of request target path (UTF-8 allowed, '+' not treated as space, invalid % -> 400)
- [ ] Server header (intentionally omitted to keep minimal)
- [ ] Access-Control-* (CORS) helpers

## Performance / architecture

- [x] Single-thread event loop (one server instance)
- [x] Horizontal scaling via SO_REUSEPORT (multi-reactor)
- [x] Multi-instance orchestration wrapper (`MultiHttpServer`) (explicit `reusePort=true` for >1 threads; aggregated stats; resolved port immediately after construction)
- [x] writev scatter-gather for response header + body
- [x] Outbound write buffering with EPOLLOUT-driven backpressure
- [x] Header read timeout (Slowloris mitigation) (configurable, disabled by default)
- [ ] Benchmarks & profiling docs
- [ ] Zero-copy sendfile() support for static files

Safety / robustness

- [x] Configurable header/body limits
- [x] Graceful shutdown loop (runUntil)
- [x] Slowloris style header timeout mitigation (implemented as header read timeout)
- [x] TLS termination (OpenSSL) with ALPN, mTLS, version bounds, handshake timeout & per-server metrics

Developer experience

- [x] Builder style HttpServerConfig
- [x] Simple lambda handler signature
- [x] Simple exact-match per-path routing (`addPathHandler`)
- [x] Configurable trailing slash handling (Strict / Normalize / Redirect)
- [x] Lightweight built-in logging (spdlog optional integration) – pluggable interface TBD
- [ ] Middleware helpers
- [ ] Pluggable logging interface (abstract sink / formatting hooks)

Misc

- [x] Move semantics for HttpServer
- [x] MultiHttpServer convenience wrapper
- [x] Compression (gzip & deflate phase 1)
- [ ] Public API stability guarantee (pre-1.0)
- [ ] License file

### Connection Close Semantics (CloseMode)

Aeronet models per-connection shutdown intent with `ConnectionState::CloseMode`:

| Mode | Meaning | Typical triggers |
|------|---------|------------------|
| `None` | Connection remains eligible for keep-alive / more requests | Normal operation |
| `DrainThenClose` | Allow pending outbound data to flush, then close | Client sent `Connection: close`; server reached `maxRequestsPerConnection`; keep-alive disabled; handler explicitly requested close |
| `Immediate` | Terminate as soon as practical (after queuing minimal error bytes) | Malformed request line / headers; mixed `Content-Length` + `Transfer-Encoding`; unsupported `Transfer-Encoding`; header/body size limits exceeded (413/431); HTTP/1.0 with TE; transport read/write failures; outbound buffer overflow; internal fatal errors |

Public helper methods on `ConnectionState`:

- `requestDrainAndClose()` – escalate from `None` to `DrainThenClose` (idempotent if already closing)
- `requestImmediateClose()` – unconditionally mark connection as `Immediate`
- `isDrainCloseRequested()` / `isImmediateCloseRequested()` / `isAnyCloseRequested()` – query helpers

The server prefers preserving response bytes for graceful conditions (client directive, normal lifecycle
limits) while using immediate teardown for hard protocol violations or transport integrity issues. Error
paths that call `emitSimpleError(..., /*immediate=*/true)` will always mark the connection `Immediate`.

User code normally does not manipulate `CloseMode` directly; returning a response with
`Connection: close` (or exhausting keep-alive criteria) automatically maps to `DrainThenClose`.
Only unrecoverable scenarios escalate to `Immediate` to avoid reusing a compromised protocol state.

### Compression (gzip, deflate, optional zstd)

Implemented capabilities:

- Formats: gzip & deflate (raw deflate wrapped by zlib) behind `AERONET_ENABLE_ZLIB` build flag.
- Optional: **zstd** behind `AERONET_ENABLE_ZSTD` (independent of zlib). If both enabled the negotiation pool becomes
  `gzip, zstd, deflate` by default (enum order); you can override with `CompressionConfig::preferredFormats`.
  (Earlier versions placed deflate before zstd; ordering changed to prefer zstd's modern ratio/latency profile.)
- Negotiation: Parses `Accept-Encoding` with q-values; chooses format with highest q (server preference breaks ties). Falls back to identity if none acceptable.
- Server preference: Order in `CompressionConfig::preferredFormats` only breaks ties among encodings with equal effective q-values; it does NOT restrict the server from selecting another supported encoding with a strictly higher q that is not listed. (If you leave the vector empty, the built‑in default order `gzip, deflate` is used for tie-breaks.)
- Threshold: `minBytes` delays compression until uncompressed size reaches threshold (streaming buffers until then; fixed responses decide immediately).
- Streaming integration: Headers are withheld until compression activation decision so `Content-Encoding` is always accurate once emitted.
- Per-response opt-out: supply your own `Content-Encoding` header (e.g. `identity` to disable, or a custom value you fully manage). If present, Aeronet never applies automatic compression and does not modify your header/body.
- Vary header: Adds `Vary: Accept-Encoding` when compression applied (configurable via `addVaryHeader`).
- Identity safety: If threshold not met, buffered bytes are flushed uncompressed and no misleading `Content-Encoding` is added.
- Q-value precedence: Correctly honors client preference (e.g. `gzip;q=0.1, deflate;q=0.9` selects deflate even if server lists gzip first).
- Explicit identity rejection: If a client sends an `Accept-Encoding` header that (a) explicitly forbids the
  identity coding via `identity;q=0` and (b) does not list any supported compression coding with a positive `q`
  (and wildcard `*` does not introduce one), Aeronet now responds with **`406 Not Acceptable`** and a short plain
  text body `"No acceptable content-coding available"`. This follows RFC 9110 §12.5.3 guidance that a server MAY
  reject a request when none of the client's acceptable codings are available. Typical scenario:
  `Accept-Encoding: identity;q=0, br;q=0` (when brotli is unsupported) → 406. If any supported coding is acceptable
  (e.g. `identity;q=0, gzip`), normal negotiation proceeds and that coding is used.

Zstd tuning (when enabled):

```cpp
CompressionConfig cfg;
cfg.zstd.compressionLevel = 5;   // default ~3
cfg.zstd.windowLog = 0;          // 0 => library default; set >0 (e.g. 23) to force max window
```

`compressionLevel` maps to the standard zstd levels (higher = more CPU, usually better ratio). `windowLog` controls
the maximum back‑reference window; leave at 0 unless you need deterministic memory limits.

Sample multi-line version string fragment (with TLS, logging, and both compression libs enabled):

```text
aeronet 0.1.0
  tls: OpenSSL 3.0.13 30 Jan 2024
  logging: spdlog 1.15.3
  compression: zlib 1.2.13, zstd 1.5.6
```

Planned / future:

- Additional formats (brotli) behind separate feature flags.
- Content-Type allowlist enforcement (framework in config; default list to be finalized).
- Compression ratio metrics in `RequestMetrics`.
- Adaptive buffer sizing & memory pooling for encoder contexts.

Minimal usage example:

```cpp
CompressionConfig c;
c.minBytes = 64;
c.preferredFormats.push_back(Encoding::gzip);
c.preferredFormats.push_back(Encoding::deflate);
HttpServerConfig cfg;
cfg.withCompression(c);
HttpServer server(cfg);
server.setHandler([](const HttpRequest&) {
  return HttpResponse(200, "OK").contentType("text/plain").body(std::string(1024, 'A'));
});
```

For streaming:

```cpp
server.setStreamingHandler([](const HttpRequest&, HttpResponseWriter& w){
  w.statusCode(200, "OK");
  w.contentType("text/plain");
  for (int i=0;i<10;++i) {
    w.write(std::string(50,'x')); // accumulates until threshold then switches to compressed chunks
  }
  w.end();
});
```

### Reserved Headers

The library intentionally reserves a small set of response headers that user code cannot set directly on
`HttpResponse` (fixed responses) or via `HttpResponseWriter` (streaming) because Aeronet itself manages them or
their semantics would be invalid / ambiguous without deeper protocol features:

Reserved now (assert if attempted in debug; ignored in release for streaming):

- `Date` – generated once per second and injected automatically.
- `Content-Length` – computed from the body (fixed) or set through `setContentLength()` (streaming). Prevents
  inconsistencies between declared and actual size.
- `Connection` – determined by keep-alive policy (HTTP version, server config, request count, errors). User code
  supplying conflicting values could desynchronize connection reuse logic.
- `Transfer-Encoding` – controlled by streaming writer (`chunked`) or omitted when `Content-Length` is known. Allowing
  arbitrary values risks illegal CL + TE combinations or unsupported encodings.
- `Trailer`, `TE`, `Upgrade` – not yet supported by Aeronet; reserving them now avoids future backward-incompatible
  behavior changes when trailer / upgrade features are introduced.

Allowed convenience helpers:

- `Content-Type` via `contentType()` or `setHeader("Content-Type", ...)` in streaming.
- `Location` via `location()` for redirects.

All other headers (custom application / caching / CORS / etc.) may be freely set; they are forwarded verbatim.
This central rule lives in a single helper (`HttpResponse::IsReservedHeader`).

### Request Header Duplicate Handling

Incoming request headers are parsed into a flat buffer and exposed through case‑insensitive lookups on
`HttpRequest`. Aeronet applies a deterministic, allocation‑free in‑place policy when a duplicate request
header field name is encountered while parsing. The policy is driven by a constexpr classification table that maps well‑known header names (case‑insensitive) to one of the following behaviors:

| Policy Code | Meaning | Examples |
|-------------|---------|----------|
| `,` | List merge: append a comma and the new non‑empty value | `Accept`, `Accept-Encoding`, `Via`, `Warning`, `TE` |
| `;` | Cookie merge: append a semicolon (no extra space) | `Cookie` |
| (space) | Space join: append a single space and the new non‑empty value | `User-Agent` |
| `O` | Override: keep ONLY the last occurrence (replace existing value, no concatenation) | `Authorization`, `Range`, `From`, conditional time headers |
| `\0` | Disallowed duplicate: second occurrence triggers `400 Bad Request` | `Content-Length`, `Host` |

Fallback for unknown (unclassified) headers currently assumes list semantics (`,`). This is configurable
internally (a server config flag exists for future tightening) and is chosen to preserve extension /
experimental headers that follow conventional `1#token` or `1#element` ABNF patterns.

Merging rules are value‑aware:

- If the existing stored value is empty and a later non‑empty value arrives, the new value replaces it
  (no leading separator is inserted).
- If the new value is empty and the existing value is non‑empty, no change is made (we avoid trailing
  separators that would manufacture an empty list member).
- Only when both values are non‑empty is the separator inserted (`,` / `;` / space) followed by the new
  bytes.
- Override (`O`) headers always adopt the last (even if empty → empty replaces previous non‑empty).

Implementation details:

1. The first occurrence of each header stores `name` and `value` as `std::string_view` slices into the
   connection read buffer (no copy).
2. On a mergeable duplicate, the new value bytes are temporarily copied into a scratch buffer, the tail
   of the original buffer is shifted right with a single `memmove`, and the separator plus new value are
   written into the gap. All subsequent header string_views are pointer‑adjusted (stable hashing / equality
   are preserved because key characters do not change, only their addresses move uniformly).
3. Override simply rebinds the existing `value` view to point at the newest occurrence (no buffer mutation).
4. Disallowed duplicates short‑circuit parsing and return `400 Bad Request` immediately.

Security / robustness notes:

- Disallowing duplicate `Content-Length` and `Host` prevents common request smuggling vectors that rely on
  conflicting or ambiguous canonicalization rules across intermediaries.
- A future stricter mode may treat unknown header duplicates as disallowed instead of comma‑merging; the
  hook for that decision exists in the classification fallback.
- The implementation never allocates proportional to header count on a merge path; each merge performs at
  most one temporary copy (size of the new value) plus one tail shift.

Examples:

```text
Accept: text/plain
Accept: text/html
→ Accept: text/plain,text/html

Authorization: Bearer token1
Authorization: Bearer token2
→ Authorization: Bearer token2

Cookie: a=1
Cookie: b=2
→ Cookie: a=1;b=2

User-Agent: Foo
User-Agent: Bar
→ User-Agent: Foo Bar

Host: example.com
Host: other
→ 400 Bad Request (duplicate forbidden)
```

Tests covering all policy branches (list, cookie, space, override, disallowed, empty edge cases, case
insensitivity) live in the lightweight `http-request_test.cpp` suite to keep feedback tight.

### HttpResponse Design & Performance

`HttpResponse` (fixed / non‑streaming responses) builds the entire status line, headers and body inside a **single
contiguous dynamically grown buffer**. This minimizes allocations, maximizes cache locality, and keeps syscall layer
simple (a single `writev` pairs the header block with the body when streaming is not used).

Memory layout (before any user header is appended):

```text
[HTTP/1.1 SP status-code [SP reason] CRLF][CRLF][CRLF]
                            ^        ^   ^
                            |        |   +-- DoubleCRLF sentinel (always present)
                            |        +------ End of status / optional reason line
                            +--------------- First reason character (if any)
```

After headers (each user header prepends a CRLF and shifts only the tail once):

```text
Status/Reason CRLF (CRLF Name ": " Value)* CRLF CRLF [Body]
```

Key properties:

- **Single allocation growth** – headers and body share one buffer (no per‑header node allocations).
- **Append fast path** – `appendHeader()` does not scan; it inserts `CRLF + key + ": " + value` before the trailing
  DoubleCRLF, shifting only the (DoubleCRLF + body) tail (O(bodyLen)). Duplicate headers are allowed intentionally.
- **Uniqueness path** – `header()` performs a linear scan to find an existing key at CRLF‑delimited line starts **using
  case‑insensitive comparison of the header name (RFC 7230 token rules)**; if found it replaces the value in place (one
  `memmove` for size delta). If not found it falls back to `appendHeader()`. The original casing of the first
  occurrence is preserved (subsequent replacements do not alter its characters) so you can freely call `header()` with
  any casing (`"Content-Type"`, `"content-type"`, `"CONTENT-TYPE"`).
- **Reason phrase mutation** – `reason()` can grow, shrink, add or remove the reason phrase. It performs at most one
  tail shift and updates internal offsets for headers/body.
- **Body mutation safety** – `body()` overwrites in place and grows capacity exponentially. If the source string_view
  points inside the internal buffer (e.g. using part of the reason or an earlier body) the implementation preserves
  safety across potential reallocation.
- **Value‑category preserving fluent API** – All mutators (`statusCode`, `reason`, `body`, `appendHeader`, `header`,
   `contentType`, `location`) have both `&` and `&&` ref‑qualified overloads so chains on temporaries don’t force an
   intermediate named variable:

```cpp
return HttpResponse(404)
    .reason("Not Found")
    .contentType("text/plain")
    .body("missing\n");
```

- **Post‑finalize immutability** – `finalizeAndGetFullTextResponse()` injects reserved headers (`Date`, `Connection`,
  and `Content-Length` if body non‑empty). After it returns further mutation is undefined behavior (would duplicate
  reserved headers or corrupt layout). Call it exactly once per response.
- **Reserved header enforcement** – Centralized in `HttpResponse::IsReservedHeader` (same rule used by streaming
  writer). Attempting to set one triggers an assertion in debug builds.

Complexity summary (amortized):

| Operation          | Complexity | Notes |
|--------------------|------------|-------|
| `statusCode()`     | O(1)       | Overwrites 3 digits |
| `reason()`         | O(trailing) | One tail `memmove` if size delta |
| `appendHeader()`   | O(bodyLen) | Shift tail once; no scan |
| `header()`         | O(headers + bodyLen) | Linear scan + maybe one shift |
| `body()`           | O(delta) + realloc | Exponential growth strategy |
| `finalize*()`      | O(reserved count) | Appends small, bounded set |

Testing highlights:

- Header replacement: larger, smaller, same length, with and without body.
- Reason growth/shrink with headers present & absent (including removal to empty and re‑addition).
- Fuzz test generating random sequences of operations (status, reason, body, header additions & replacements).
- Safety test where `body()` receives a view referencing internal buffer memory (reallocation correctness).

Usage guidelines:

- Use `appendHeader()` when duplicates are acceptable (cheapest path).
- Use `header()` only when you must guarantee uniqueness. Matching is case‑insensitive; prefer a canonical style (e.g.
  `Content-Type`) for readability, but behavior is the same regardless of input casing.
- Chain on temporaries for concise construction; the rvalue-qualified overloads keep the object movable.
- Finalize exactly once right before sending.

Future possible extensions (not yet implemented): transparent compression insertion, zero‑copy file send mapping,
and an alternate layout for extremely large header counts.

### MultiHttpServer Lifecycle & reusePort Requirement

`MultiHttpServer` constructs all underlying `HttpServer` instances immediately. If `cfg.port == 0` (ephemeral) the
first underlying server binds and resolves the concrete port during construction, so `multi.port()` is valid right
after the constructor returns. `start()` only launches the event loop threads – no busy-wait for port discovery.

Explicit `reusePort` policy:

- For `threadCount > 1` you MUST set `cfg.reusePort = true` beforehand (otherwise the constructor throws `invalid_argument`).
- For a single thread (`threadCount == 1`) `reusePort` is optional.

Move semantics: moving a `MultiHttpServer` after `start()` is forbidden (debug assert) because worker threads capture
raw pointers to the internal `HttpServer` objects.

Example:

```cpp
HttpServerConfig cfg;
cfg.port = 0;          // ephemeral
cfg.reusePort = true;  // required for >1 threads
MultiHttpServer multi(cfg, 4); // port resolved here
std::cout << multi.port() << "\n"; // valid now
multi.setHandler(...);
multi.start();
```

## TLS Features (Current)

TLS support is optional (`AERONET_ENABLE_OPENSSL`). When configured via `HttpServerConfig::TLSConfig`, the following capabilities are available:

| Capability | Status | Notes |
|------------|--------|-------|
| TLS termination | ✅ | File or in‑memory PEM cert/key |
| mTLS (request) | ✅ | `withTlsRequestClientCert()` (non-fatal absence) |
| mTLS (require) | ✅ | `withTlsRequireClientCert()` (fatal if absent / invalid) |
| ALPN negotiation | ✅ | Ordered list via `withTlsAlpnProtocols()` |
| Strict ALPN enforcement | ✅ | `withTlsAlpnMustMatch(true)` -> fatal if no overlap |
| Negotiated ALPN in request | ✅ | `HttpRequest::alpnProtocol` |
| Negotiated cipher & version | ✅ | `HttpRequest::{tlsCipher,tlsVersion}` |
| Handshake logging | ✅ | `withTlsHandshakeLogging()` (cipher, version, ALPN, peer subject) |
| Min / Max protocol version | ✅ | `withTlsMinVersion("TLS1.2")`, `withTlsMaxVersion("TLS1.3")` |
| Handshake timeout | ✅ | `withTlsHandshakeTimeout(ms)` closes stalled handshakes |
| Graceful TLS shutdown | ✅ | Best‑effort `SSL_shutdown` before close |
| ALPN strict mismatch counter | ✅ | Per‑server stats |
| Handshake success counter | ✅ | Per‑server stats |
| Client cert presence counter | ✅ | Per‑server stats |
| ALPN distribution | ✅ | Vector (protocol,count) in stats |
| TLS version distribution | ✅ | Stats field |
| Cipher distribution | ✅ | Stats field |
| Handshake duration metrics | ✅ | Count / total ns / max ns |
| JSON stats export | ✅ | `serverStatsToJson()` includes TLS metrics |
| No process‑global mutable TLS state | ✅ | All metrics per server instance |
| Session resumption | ⏳ | Planned |
| SNI multi-cert routing | ⏳ | Planned |
| Hot cert/key reload | ⏳ | Planned |
| OCSP / revocation | ⏳ | Planned |

### TLS Configuration Example

```cpp
HttpServerConfig cfg;
cfg.withPort(8443)
   .withTlsCertKeyMemory(certPem, keyPem)
   .withTlsAlpnProtocols({"http/1.1"})
   .withTlsAlpnMustMatch(true)
   .withTlsMinVersion("TLS1.2")
   .withTlsMaxVersion("TLS1.3")
   .withTlsHandshakeTimeout(std::chrono::milliseconds(750))
   .withTlsHandshakeLogging();

HttpServer server(cfg);
server.setHandler([](const HttpRequest& req){
  HttpResponse r{200, "OK"};
  r.contentType = "text/plain";
  r.body = std::string("cipher=") + std::string(req.tlsCipher)
         + " version=" + std::string(req.tlsVersion)
         + " alpn=" + std::string(req.alpnProtocol);
  return r;
});
server.run();
```

### Accessing TLS Metrics

```cpp
#include <print>
auto st = server.stats();
std::print("handshakes={} clientCerts={} alpnStrictMismatches={}\n",
           st.tlsHandshakesSucceeded,
           st.tlsClientCertPresent,
           st.tlsAlpnStrictMismatches);
for (auto& [proto,count] : st.tlsAlpnDistribution) {
  std::print("ALPN {} -> {}\n", proto, count);
}
for (auto& [ver,count] : st.tlsVersionCounts) {
  std::print("Version {} -> {}\n", ver, count);
}
double avgNs = st.tlsHandshakeDurationCount ?
               double(st.tlsHandshakeDurationTotalNs) / st.tlsHandshakeDurationCount : 0.0;
std::print("avgHandshakeNs={}\n", avgNs);
```

---

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
| Parsing | Percent-decoding of path | ✅ | `http_url_decoding.cpp` (spaces, UTF-8, '+', invalid percent -> 400) |
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

## Build & Installation

Full, continually updated build, install, and package manager instructions live in [`docs/INSTALL.md`](docs/INSTALL.md).

Quick start (release build of examples):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

For TLS toggles, sanitizers, Conan/vcpkg usage and `find_package` examples, see the INSTALL guide.

## Trailing Slash Policy

`HttpServerConfig::TrailingSlashPolicy` controls how paths that differ only by a single trailing `/` are treated.

Resolution algorithm (applies to all policies):

1. Always attempt an exact match first. If the incoming target exactly equals a registered path, that handler is used and the policy does not intervene. (So if both `/foo` and `/foo/` are registered you get whichever you requested, under every policy.)
2. If no exact match:
   - If the request ends with a single trailing slash (excluding root `/`) and the canonical form without that slash exists:
     - Strict   – 404 (variants are distinct; no mapping)
     - Normalize – treat as the canonical path (strip the slash internally, no redirect)
     - Redirect – emit `301 Moved Permanently` with `Location: /foo`
   - Else if the request does not end with a slash, policy is Normalize, and only the slashed variant exists (e.g. only `/foo/` registered): dispatch to that variant (symmetry in the opposite direction)
   - Otherwise: 404
3. The root path `/` is never redirected or normalized.

Behavior summary:

| Policy    | `/foo` registered only | `/foo/` registered only | Both registered      |
|-----------|------------------------|--------------------------|----------------------|
| Strict    | `/foo/` -> 404         | `/foo` -> 404            | Each exact served    |
| Normalize | `/foo/` -> serve `/foo`| `/foo` -> serve `/foo/`  | Each exact served    |
| Redirect  | `/foo/` -> 301 `/foo`  | `/foo` -> 404            | Each exact served (no redirect) |

Usage:

```cpp
HttpServerConfig cfg;
cfg.withTrailingSlashPolicy(HttpServerConfig::TrailingSlashPolicy::Redirect);
HttpServer server(cfg);
server.addPathHandler("/foo", http::Method::GET, [](const HttpRequest&){
  HttpResponse r{200, "OK"}; r.body="foo"; r.contentType="text/plain"; return r; });
```

Tests covering this matrix live in `tests/http_trailing_slash.cpp`.

## Construction Model (RAII) & Ephemeral Ports

`HttpServer` binds, sets socket options, enters listening state, and registers the listening fd with epoll inside its constructor (RAII). If you request an ephemeral port (`port = 0` in `HttpServerConfig`), the kernel-assigned port is immediately available via `server.port()` after construction (no separate `setupListener()` call required – that legacy function was removed during refactor).

Why RAII?

- Guarantees a fully initialized, listening server object or a thrown exception (no half-initialized state)
- Simplifies lifecycle (no forgotten setup step)
- Enables immediate test usage with ephemeral ports

Ephemeral port pattern in tests / examples:

```cpp
HttpServerConfig cfg; // let kernel choose the port
HttpServer server(cfg);
uint16_t actual = server.port(); // resolved port
```

NOTE: A previous experimental non-throwing `tryCreate` factory was removed to reduce API surface; the throwing constructor is the only creation path for now.

## Quick Usage Examples

### Minimal Global Handler (Ephemeral Port)

```cpp
#include <aeronet/aeronet.hpp>
#include <print>
using namespace aeronet;

int main() {
  HttpServer server(cfg.withPort(8080));
  std::print("Listening on {}\n", server.port());
  server.setHandler([](const HttpRequest& req) {
    return HttpResponse(200, "OK").body("Hello from Aeronet\n").contentType("text/plain");
  });
  server.run(); // press Ctrl+C to terminate process
}
```

### Per-Path Routing & Method Masks

```cpp
HttpServer server(HttpServerConfig{}); // ephemeral port

server.addPathHandler("/hello", http::MethodSet{http::Method::GET}, [](const HttpRequest&){
  HttpResponse r; r.statusCode=200; r.reason="OK"; r.contentType="text/plain"; r.body="world"; return r; });

// Add POST later (merges methods)
server.addPathHandler("/hello", http::Method::POST, [](const HttpRequest& req){
  HttpResponse r; r.statusCode=200; r.reason="OK"; r.contentType="text/plain"; r.body=req.body; return r; });

// Unknown path -> 404, known path wrong method -> 405 automatically.

server.run();
```

### Multi‑Reactor (SO_REUSEPORT) Launch Sketch

```cpp
std::vector<std::jthread> threads;
for (int i = 0; i < 4; ++i) {
  threads.emplace_back([i]{
  HttpServerConfig cfg; cfg.withPort(8080).withReusePort(true); // or 0 for ephemeral resolved separately
    HttpServer s(cfg);
    s.setHandler([](const HttpRequest&){ HttpResponse r{200, "OK"}; r.body="hi"; r.contentType="text/plain"; return r; });
  s.run();
  });
}
```

### Accessing Backpressure / IO Stats

```cpp
auto st = server.stats();
std::print("queued={} imm={} flush={} defer={} cycles={} maxConnBuf={}\n",
  st.totalBytesQueued,
  st.totalBytesWrittenImmediate,
  st.totalBytesWrittenFlush,
  st.deferredWriteEvents,
  st.flushCycles,
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

### Accessing the Query String & Parameters

Each `HttpRequest` exposes:

- `path()`        : URL-decoded path (RFC 3986 percent-decoded in a single pass; invalid escape -> 400)
- `method()`      : HTTP request method
- `query()`       : raw query substring (no leading `?`), NOT percent-decoded wholesale
- `version()`     : HTTP version (typed)
- `queryParams()` : lightweight forward range over decoded `(key,value)` pairs

Decoding strategy:

1. The request target is split once on the first `?`.
2. The path segment is percent-decoded in-place (one pass). Any invalid escape produces a 400 error.
3. The raw query segment is left untouched (so delimiters `&` and `=` remain unambiguous even if later a malformed escape exists).
4. Iteration via `queryParams()` performs component-wise decoding:
   - Split on `&` into tokens.
   - For each token split at the first `=` (missing `=` ⇒ empty value).
   - Percent-decode key and value separately; invalid / incomplete escapes are left verbatim (no rejection).
   - Convert `+` to space in both key and value (application/x-www-form-urlencoded semantics).
   - Yield `std::string_view` pairs referencing either original buffer slices (no escapes) or small internal decode storage (escapes present). Copy if you need persistence.

Semantics & edge cases:

- Order preserved; duplicates preserved (`a=1&a=2`).
- `a` ⇒ `(a, "")`; `a=` ⇒ `(a, "")`; `=v` ⇒ `( "", v )`.
- Empty query (`/p` or `/p?`) ⇒ empty range.
- Malformed escapes in query components (`%A`, solitary `%`, `%ZZ`) are surfaced literally.
- `'+'` is translated to space only for query parameter keys/values (path keeps literal `+`).

Example:

```cpp
void handler(const aeronet::HttpRequest& req) {
  for (auto [k,v] : req.queryParams()) {
    std::print("{} => {}\n", k, v);
  }
}
```

Characteristics summary:

| Aspect              | Path                                | Query param keys/values                      |
|---------------------|-------------------------------------|----------------------------------------------|
| Decode granularity  | Whole path once                     | Per key and per value                        |
| Invalid escapes     | 400 Bad Request                     | Left verbatim                                |
| '+' handling        | Preserved as '+'                    | Decoded to space                             |
| Duplicates          | N/A                                 | Preserved in order                           |
| Missing '='         | N/A                                 | Value = ""                                   |
| Empty key           | N/A                                 | Allowed (`=v`)                               |
| Malformed escapes   | Rejected                            | Surfaced literally                           |

Instead of manually creating N threads and N `HttpServer` instances, you can use `MultiHttpServer` to spin up a "farm" of identical servers on the same port. It:

- Accepts a base `HttpServerConfig` (set `port=0` for ephemeral bind; the chosen port is propagated to all instances)
- Forces `reusePort=true` automatically when thread count > 1
- Replicates either a global handler or all registered path handlers across each underlying server
- Exposes `stats()` returning both per-instance and aggregated totals (sums; `maxConnectionOutboundBuffer` is a max)
- Manages lifecycle with internal `std::jthread`s; `stop()` requests shutdown of every instance
- Provides the resolved listening `port()` after start (even for ephemeral port 0 requests)

Minimal example:

```cpp
#include <aeronet/aeronet.hpp>
using namespace aeronet;

int main() {
  HttpServerConfig cfg; cfg.port = 0; cfg.reusePort = true; // ephemeral, auto-propagated
  MultiHttpServer multi(cfg, 4); // 4 underlying event loops
  multi.setHandler([](const HttpRequest& req){
    return HttpResponse(200, "OK").body("hello\n").contentType("text/plain");
  });
  multi.start();
  std::print("Listening on {}\n", multi.port());
  // ... run until external signal ...
  std::this_thread::sleep_for(std::chrono::seconds(30));
  auto agg = multi.stats();
  std::print("instances={} queued={}\n", agg.per.size(), agg.total.totalBytesQueued);
  multi.stop();
}
```

### Choosing Between HttpServer, AsyncHttpServer, and MultiHttpServer

| Variant | Header | Launch API | Blocking? | Threads Created Internally | Scaling Model | Typical Use Case | Notes |
|---------|--------|-----------|-----------|-----------------------------|---------------|------------------|-------|
| `HttpServer` | `aeronet/http-server.hpp` | `run()` / `runUntil(pred)` | Yes (caller thread blocks) | 0 | Single reactor | Dedicated thread you manage or simple main-thread server | Minimal overhead |
| `AsyncHttpServer` | `aeronet/async-http-server.hpp` | `start()`, `startUntil(pred)`, `requestStop()`, `stopAndJoin()` | No | 1 `std::jthread` | Single reactor (owned) | Need non-blocking single server with safe lifetime | Owns server; access via `async.server()` |
| `MultiHttpServer` | `aeronet/multi-http-server.hpp` | `start()`, `stop()` | No | N (`threadCount`) | Horizontal SO_REUSEPORT multi-reactor | Scale across cores quickly | Replicates handlers pre-start |

Decision heuristics:

- Use `HttpServer` when you already own a thread (or can just block main) and want minimal abstraction.
- Use `AsyncHttpServer` when you want a single server but need the calling thread free (e.g. integrating into a service hosting multiple subsystems, or writing higher-level control logic while serving traffic).
- Use `MultiHttpServer` when you need multi-core throughput with separate event loops per core; simplest horizontal scale path before more advanced worker models.

Blocking semantics summary:

- `HttpServer::run()` / `runUntil()` – fully blocking; returns only on stop/predicate.
- `AsyncHttpServer::start()` – non-blocking; lifecycle controlled via stop token + `server.stop()`.
- `MultiHttpServer::start()` – non-blocking; returns after all reactors launched.

### AsyncHttpServer (Single-Reactor Background Wrapper)

```cpp
#include <aeronet/aeronet.hpp>
using namespace aeronet;

int main() {
  HttpServerConfig cfg; cfg.withPort(0); // ephemeral
  HttpServer server(cfg);
  server.setHandler([](const HttpRequest&){ return HttpResponse(200, "OK").body("hi").contentType("text/plain"); });

  AsyncHttpServer async(std::move(server));
  async.start();
  // main thread free to do orchestration / other work
  std::this_thread::sleep_for(std::chrono::seconds(2));
  async.requestStop();
  async.stopAndJoin();
  async.rethrowIfError();
}
```

Predicate form (stop when external flag flips):

```cpp
std::atomic<bool> done{false};
AsyncHttpServer async(std::move(server));
async.startUntil([&]{ return done.load(); });
// later
done = true; // loop exits soon (bounded by poll interval)
async.stopAndJoin();
```

Notes:

- Do not call `run()` directly on the underlying `HttpServer` while an `AsyncHttpServer` is active.
- Register handlers before `start()` unless you provide external synchronization for modifications.
- `stopAndJoin()` is idempotent; destructor performs it automatically as a safety net.

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
  std::print("[srv{}] queued={} imm={} flush={}\n", i,
             s.totalBytesQueued,
             s.totalBytesWrittenImmediate,
             s.totalBytesWrittenFlush);
}
```

### Logging

Logging uses `spdlog` if `AERONET_ENABLE_SPDLOG` is defined at build time; otherwise a lightweight fallback provides the same call style (`log::info("message {}", value)`). The fallback uses `std::vformat` when available and degrades gracefully if formatting fails (appends arguments). Timestamps are ISO 8601 UTC with millisecond precision. Levels: trace, debug, info, warn, error, critical. You can adjust level in fallback with `aeronet::log::set_level(aeronet::log::level::debug);`.

Pluggable logging sinks / structured logging hooks are planned; current design keeps logging dependency-free by default.

## Streaming Responses (Chunked / Incremental)

The streaming API lets a handler produce a response body incrementally without a priori knowing its full size.
Register a streaming handler instead of the fixed response/global or per-path handlers:

```cpp
HttpServer server(HttpServerConfig{}.withPort(8080));
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
  Additional guarantees (see in-code docs for `HttpResponseWriter::end()`):
  - Idempotent: repeated calls are ignored after the first.
  - Ensures headers are emitted (lazy strategy) and flushes any buffered pre-compression bytes.
  - Flushes final compressor bytes if compression activated, then emits last chunk when chunked.
  - For fixed `Content-Length` responses (when declared), does not pad or truncate; debug builds assert the exact
    body byte count (identity or user-encoded) matches the declared length.
  - For HEAD requests: still sends headers (with synthesized `Content-Length` if none provided) but suppresses body.
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
- Mixed / precedence / conflict / HEAD suppression / keep-alive mixing: `tests/http_streaming_mixed.cpp`

### Mixed Mode & Dispatch Precedence

You can register both fixed (normal) and streaming handlers simultaneously at different granularity levels. The server applies the following precedence when selecting which handler to invoke for a request method + path:

1. Path-specific streaming handler (highest)
2. Path-specific normal handler
3. Global streaming handler
4. Global normal handler (lowest)

If no handler matches the path: 404. If a path exists but the method is not in its allow-set: 405 (Method Not Allowed). Method allow sets for streaming and normal handlers are independently validated to prevent duplicate conflicting registration (conflicts throw early at registration time).

HEAD requests participate in the same precedence logic using an implicit fallback to GET when a HEAD-specific registration is absent (common ergonomic shortcut). Body data generated by streaming handlers is automatically suppressed for HEAD while preserving `Content-Length` if it was explicitly set.

Conflict rules:

- Registering a streaming handler for (path, method) that already has a normal handler (or vice-versa) throws.
- Distinct method sets on the same path can split between streaming and normal handlers (e.g., GET streaming, POST normal) enabling flexible composition.

Example (mixed per-path + global fallback):

```cpp
HttpServer server(HttpServerConfig{});
// Global normal fallback
server.setHandler([](const HttpRequest&){ HttpResponse r{200, "OK"}; r.contentType="text/plain"; r.body="GLOBAL"; return r; });
// Global streaming fallback (higher than global normal)
server.setStreamingHandler([](const HttpRequest&, HttpResponseWriter& w){
  w.setStatus(200, "OK"); w.setHeader("Content-Type", "text/plain"); w.write("STREAMFALLBACK"); w.end();
});
// Path-specific streaming (highest precedence for GET)
http::MethodSet getOnly; getOnly.insert(http::Method::GET);
server.addPathStreamingHandler("/stream", getOnly, [](const HttpRequest&, HttpResponseWriter& w){
  w.setStatus(200, "OK"); w.setHeader("Content-Type", "text/plain"); w.write("PS"); w.end();
});
// Path-specific normal (takes precedence over global fallbacks for POST)
http::MethodSet postOnly; postOnly.insert(http::Method::POST);
server.addPathHandler("/stream", postOnly, [](const HttpRequest&){ return HttpResponse{201, "Created", "text/plain", "NORMAL"}; });
```

Behavior summary for above:

- `GET /stream` -> path streaming handler (body "PS")
- `POST /stream` -> path normal handler (body "NORMAL")
- `GET /other` -> global streaming fallback (body "STREAMFALLBACK")
- If global streaming handler were absent: `GET /other` would use global normal fallback (body "GLOBAL")

See `tests/http_streaming_mixed.cpp` for exhaustive precedence, conflict, HEAD-suppression, keep-alive mixed sequencing, and 405 validations.

## Configuration API (builder style)

`HttpServerConfig` lives in `aeronet/http-server-config.hpp` and exposes fluent setters (withX naming):

```cpp
HttpServerConfig cfg;
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

### Limits

- 431 is returned if the header section exceeds `maxHeaderBytes`.
- 413 is returned if the declared `Content-Length` exceeds `maxBodyBytes`.
- Connections exceeding `maxOutboundBufferBytes` (buffered pending write bytes) are marked to close after flush (default 4MB) to prevent unbounded memory growth if peers stop reading.
- Slowloris protection: configure `withHeaderReadTimeout(ms)` to bound how long a client may take to send an entire request head (request line + headers). 0 disables.

### Performance / Metrics & Backpressure

`HttpServer::stats()` exposes aggregated counters:

- `totalBytesQueued` – bytes accepted into outbound buffering (including those sent immediately)
- `totalBytesWrittenImmediate` – bytes written synchronously on first attempt (no buffering)
- `totalBytesWrittenFlush` – bytes written during later flush cycles (EPOLLOUT)
- `deferredWriteEvents` – number of times EPOLLOUT was registered due to pending data
- `flushCycles` – number of flush attempts triggered by writable events
- `maxConnectionOutboundBuffer` – high-water mark of any single connection's buffered bytes

Use these to gauge backpressure behavior and tune `maxOutboundBufferBytes`. When a connection's pending buffer would exceed the configured maximum, it is marked for closure once existing data flushes, preventing unbounded memory growth under slow-reader scenarios.

### Metrics Callback (Scaffold)

You can install a lightweight per-request metrics callback capturing basic timing and size information:

```cpp
server.setMetricsCallback([](const HttpServer::RequestMetrics& m){
  // Export to stats sink / log
  // m.method, m.target, m.status, m.bytesIn, m.bytesOut (currently 0 for fixed responses), m.duration, m.reusedConnection
});
```

Current fields (alpha – subject to change before 1.0):

| Field | Description |
|-------|-------------|
| method | Original request method string |
| target | Request target (decoded path) |
| status | Response status code (best-effort 200 for streaming if not overridden) |
| bytesIn | Request body size (after chunk decode) |
| bytesOut | Placeholder (0 for now, future: capture flushed bytes per response) |
| duration | Wall time from parse completion to response dispatch end (best effort) |
| reusedConnection | True if this connection previously served other request(s) |

The callback runs in the event loop thread – keep it non-blocking.

### Test HTTP Client Helper

The test suite uses a unified helper for simple GETs, streaming incremental reads, and multi-request keep-alive batches. See `docs/test-client-helper.md` for guidance when adding new tests.

### Roadmap additions

- [x] Connection write buffering / partial write handling
- [x] Outgoing chunked responses & streaming interface (phase 1)
- [ ] Trailing headers exposure for chunked requests
- [ ] Richer routing (wildcards, parameter extraction)
- [x] TLS (OpenSSL) support (basic HTTPS termination)
- [ ] Benchmarks & perf tuning notes

### TLS (HTTPS) Support

TLS termination is optional and enabled at build time with the CMake option `AERONET_ENABLE_OPENSSL=ON` (default ON in main project builds). When enabled, a dedicated `aeronet_tls` module is compiled and linked; the core library avoids including OpenSSL headers directly (boundary kept inside the TLS module).

Enable at configure time:

```bash
cmake -S . -B build -DAERONET_ENABLE_OPENSSL=ON
cmake --build build -j
```

Configure a server with certificate + key (filesystem paths):

```cpp
using namespace aeronet;
HttpServerConfig cfg;
cfg.withPort(0) // ephemeral
  .withTlsCertKey("/path/to/server.crt", "/path/to/server.key");
HttpServer server(cfg);
server.setHandler([](const HttpRequest&){ HttpResponse r{200, "OK"}; r.body="secure"; r.contentType="text/plain"; return r; });
server.run();
```

Client example:

```bash
curl -k https://localhost:<port>/
```

In-memory (no temp files) certificate + key provisioning (e.g. when you already hold PEM material in memory):

```cpp
using namespace aeronet;
// Suppose certPem and keyPem are std::string containing PEM blocks
HttpServerConfig cfg;
cfg.withPort(0)
  .withTlsCertKeyMemory(certPem, keyPem);
HttpServer server(cfg);
```

If you need to dynamically generate a self-signed cert at runtime (tests, ephemeral dev), create it with OpenSSL APIs
then pass the resulting PEM strings via `withTlsCertKeyMemory` (the test helper `tests/test_tls_helper.hpp` shows a reference implementation).

Notes:

- If you supply TLS configuration (`withTlsCertKey` or `withTlsCertKeyMemory`) but the library was built without OpenSSL, the constructor throws.
- Optional: `withTlsCipherList("HIGH:!aNULL:!MD5")` to tune ciphers; empty string => OpenSSL default.
- `withTlsRequestClientCert(true)` sets the server to *request* (but not fail without) a client certificate.
- `withTlsRequireClientCert(true)` enables strict mTLS: handshake aborts if the client does not present a cert or it fails verification.
- `withTlsAddTrustedClientCert(pem)` lets you append in-memory PEM certs to the trust store (useful for tests / pinning self-signed client roots). Call multiple times to add several.
- ALPN: `withTlsAlpnProtocols({"http/1.1"})` advertises server preference list; first overlap with client wins. Selected protocol is exposed per request via `HttpRequest::alpnProtocol`.
- Strict ALPN: `withTlsAlpnMustMatch(true)` now aborts the TLS handshake immediately (fatal alert) if no protocol overlap exists. Without strict mode the handshake proceeds without ALPN acknowledgment. A global counter of such strict mismatches is exposed via `tlsAlpnStrictMismatchCount()`.
- Roadmap (future): HTTP/2 evaluation once h2 protocol is added to ALPN list; optional OCSP stapling, richer cipher policy helpers.
- Tests use only in-memory ephemeral certs now (no checked‑in key material) for better hygiene.
- The internal event loop integrates TLS handshakes via a transport abstraction; epoll edge-triggered mechanics remain unchanged.

## License

Licensed under the MIT License. See [LICENSE](LICENSE).
