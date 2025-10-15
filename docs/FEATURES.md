# aeronet Feature Reference

Single consolidated reference for **aeronet** features.

## Index

1. [HTTP/1.1 Feature Matrix](#http11-feature-matrix)
2. [Performance / architecture](#performance--architecture)
3. [Compression & Negotiation](#compression--negotiation)
4. [Inbound Request Decompression (Config Details)](#inbound-request-decompression-config-details)
5. [Connection Close Semantics](#connection-close-semantics)
6. [Reserved & Managed Response Headers](#reserved--managed-response-headers)
7. [Request Header Duplicate Handling (Detailed)](#request-header-duplicate-handling-detailed)
8. [Query String & Parameters](#query-string--parameters)
9. [Trailing Slash Policy](#trailing-slash-policy)
10. [Construction Model (RAII & Ephemeral Ports)](#construction-model-raii--ephemeral-ports)
11. [MultiHttpServer Lifecycle](#multihttpserver-lifecycle)
12. [TLS Features](#tls-features)
13. [Streaming Responses](#streaming-responses-chunked--incremental)
14. [Mixed Mode Dispatch Precedence](#mixed-mode--dispatch-precedence)
15. [Logging](#logging)
16. [OpenTelemetry Integration](#opentelemetry-integration)
17. [Future Expansions](#future-expansions)

## HTTP/1.1 Feature Matrix

Legend: [x] implemented, [ ] planned / not yet.

### Core parsing & connection handling

- [x] Request line parsing (method, target, version)
- [x] Header field parsing (no folding / continuations)
- [x] Case-insensitive header lookup helper
- [x] Persistent connections (HTTP/1.1 default, HTTP/1.0 opt-in)
- [x] HTTP/1.0 response version preserved (no silent upgrade)
- [x] Connection: close handling
- [x] Pipelined sequential requests (no parallel handler execution)
- [x] Backpressure / partial write buffering

### Request bodies

- [x] Content-Length bodies with size limit
- [x] Chunked Transfer-Encoding decoding (request) (ignores trailers)
- [x] Content-Encoding request body decompression (gzip, deflate, zstd, multi-layer, identity skip, safety limits)
- [ ] Trailer header exposure
- [ ] Multipart/form-data convenience utilities

### Response generation

- [x] Basic fixed body responses
- [x] HEAD method (suppressed body, correct Content-Length)
- [x] Outgoing chunked / streaming responses (basic API: status/headers + incremental write + end, keep-alive capable)
- [x] Mixed-mode dispatch (simultaneous registration of streaming and fixed handlers with precedence)
- [x] Compression (gzip & deflate) (phase 1: zlib) – streaming + buffered with threshold & q-values

### Status & error handling

- [x] 400 Bad Request (parse errors, CL+TE conflict)
- [x] 413 Payload Too Large (body limit)
- [x] 431 Request Header Fields Too Large (header limit)
- [x] 501 Not Implemented (unsupported Transfer-Encoding)
- [x] 505 HTTP Version Not Supported
- [x] 400 on HTTP/1.0 requests carrying Transfer-Encoding
- [ ] 415 Unsupported Media Type (content-type based) – not required yet
- [x] 405 Method Not Allowed (enforced when path exists but method not in allow set)

### Headers & protocol niceties

- [x] Connection keep-alive / close
- [x] Content-Type (user supplied only)
- [x] Expect: 100-continue handling
- [x] Expect header ignored for HTTP/1.0 (no interim 100 sent)
- [x] Percent-decoding of request target path (UTF-8 allowed, '+' not treated as space, invalid % -> 400)
- [ ] Server header (intentionally omitted to keep minimal)
- [ ] Access-Control-* (CORS) helpers

## Performance / architecture

### Execution model & scaling

- [x] Single-thread event loop (one server instance)
- [x] Horizontal scaling via SO_REUSEPORT (multi-reactor)
- [x] Multi-instance orchestration wrapper (`MultiHttpServer`) (explicit `reusePort=true` for >1 threads; aggregated stats; resolved port immediately after construction)
- [x] writev scatter-gather for response header + body
- [x] Outbound write buffering with EPOLLOUT-driven backpressure
- [x] Header read timeout (Slowloris mitigation) (configurable, disabled by default)
- [ ] Benchmarks & profiling docs
- [ ] Zero-copy sendfile() support for static files

### Safety / robustness

- [x] Configurable header/body limits
- [x] Graceful shutdown loop (runUntil)
- [x] Slowloris style header timeout mitigation (implemented as header read timeout)
- [x] TLS termination (OpenSSL) with ALPN, mTLS, version bounds, handshake timeout & per-server metrics

### Developer experience

- [x] Builder style HttpServerConfig
- [x] Simple lambda handler signature
- [x] Simple exact-match per-path routing (`setPath`)
- [x] Configurable trailing slash handling (Strict / Normalize / Redirect)
- [x] Lightweight built-in logging (spdlog optional integration) – pluggable interface TBD
- [ ] Middleware helpers
- [ ] Pluggable logging interface (abstract sink / formatting hooks)

### Misc

- [x] Move semantics for HttpServer
- [x] MultiHttpServer convenience wrapper
- [x] Compression (gzip & deflate phase 1)
- [ ] Public API stability guarantee (pre-1.0)
- [ ] License file

## Compression & Negotiation

Supported (build‑flag gated): gzip, deflate (zlib), zstd, brotli.

### Outbound Response Compression

- Parses `Accept-Encoding` with q-values; highest client q wins; server preference only breaks ties.
- Threshold (`CompressionConfig::minBytes`) defers activation; streaming path buffers until threshold.
- Default server preference (tie‑break list) when not overridden:
  - zlib only: `gzip, deflate`
  - zlib + zstd: `zstd, gzip, deflate`
  - brotli + zstd + zlib: `br, zstd, gzip, deflate`
- Per-response opt‑out: user `Content-Encoding` prevents auto compression.
- Adds `Vary: Accept-Encoding` automatically (configurable) when compression applied.
- Identity rejection: forbidding `identity` with no acceptable alternative ⇒ `406 Not Acceptable`.

#### Per-Response Manual `Content-Encoding` (Automatic Compression Suppression)

When you stream or build a response using `HttpResponseWriter`, aeronet will decide whether to apply
automatic compression (based on `Accept-Encoding`, size threshold, configured preferences, and build flags).
However, if you explicitly set a `Content-Encoding` header yourself (via `customHeader()` / `contentEncoding()` or on a
fixed `HttpResponse`), aeronet treats this as a hard override and will NEVER engage its own encoder for that
response. This applies even if the header value is `identity`.

Practical implications:

| Scenario | Result |
|----------|-------|
| You set `Content-Encoding: gzip` and write pre-compressed bytes | aeronet forwards bytes verbatim; no size threshold buffering; no double compression risk |
| You set `Content-Encoding: identity` | Automatic compression fully disabled; body sent as-is |
| You set multiple encodings (e.g. `gzip, br`) | Currently respected verbatim (aeronet does not multi-encode outbound); use only a single encoding value for clarity |
| You set `Content-Length` + `Content-Encoding` | You MUST ensure the length matches the encoded payload size; aeronet does not recompute |
| You set neither header | aeronet may choose an encoding and add `Content-Encoding` + `Vary` when activating |

Detection logic (streaming path): first time a `Content-Encoding` header name is observed before headers flush, a
`_userProvidedContentEncoding` flag is latched; subsequent internal compression activation checks this flag and abort.

Edge cases & notes:

- Threshold buffering still occurs until either (a) you set your own `Content-Encoding` or (b) aeronet activates its own.
- If you mistakenly set an unsupported or misspelled value (e.g. `Content-Encoding: gziip`), aeronet will still skip auto compression and send it literally (client may misinterpret). Validation may be added later, so prefer correct tokens.
- For fixed (non-streaming) responses created via `HttpResponse`, the same rule applies: presence of `Content-Encoding` means no automatic compression layer is injected.
- `Vary: Accept-Encoding` is ONLY auto-added when aeronet itself performs outbound compression. Supplying your own `Content-Encoding` does not implicitly add `Vary` (you can add it manually if appropriate for caches).
- Supplying `Content-Encoding` does not affect inbound request body decompression logic (that is driven by the request's headers, not the response).

Minimal example (manual gzip):

```cpp
server.router().setDefault([](const HttpRequest&, HttpResponseWriter& w){
  w.statusCode(http::StatusOK);
  w.contentType(http::ContentTypeTextPlain);
  w.contentEncoding("gzip");            // suppress auto compression
  w.write(preCompressedHelloGzipBytes);  // already gzip-compressed data
  w.end();
});
```

To “force identity” even if thresholds would normally trigger compression:

```cpp
server.router().setDefault([](const HttpRequest&, HttpResponseWriter& w){
  w.contentEncoding("identity"); // blocks auto compression
  w.write(largePlainBuffer);
  w.end();
});
```

Introspecting the suppression in custom logic (advanced): `HttpResponseWriter::userProvidedContentEncoding()` exposes
the latched flag (primarily for future middleware instrumentation / metrics).

### Inbound Request Body Decompression (Symmetric Flags)

Codec flags enable BOTH outbound compression & inbound decoding. Multi-layer `Content-Encoding` chains decoded last→first with per-layer expansion & absolute size guards; successful decode removes the header before handler.

| Condition | Response |
|-----------|----------|
| Unknown coding | 415 |
| Empty / malformed token | 400 |
| Expansion / size limit exceeded | 413 |
| Corrupt compressed data | 400 |

Configuration sketch:

```cpp
CompressionConfig c; c.minBytes = 128; c.preferredFormats = {Encoding::zstd};
HttpServerConfig cfg; cfg.withCompression(c);
```

Planned: content-type allow list, encoder pooling, ratio metrics, adaptive format/level selection.

### Detailed Behavior (Compression)

Implemented capabilities:

- Formats: `gzip` & `deflate` (zlib), `zstd`, `br` (brotli) – each behind its own feature flag: `AERONET_ENABLE_ZLIB`, `AERONET_ENABLE_ZSTD`, `AERONET_ENABLE_BROTLI`.
- Enabling a format flag activates BOTH outbound response compression and inbound request body decompression for that format (symmetry keeps configuration minimal).
- Default server preference order (tie-break among equal effective q-values) when nothing specified in `CompressionConfig::preferredFormats` is: `gzip, deflate` if only zlib enabled; `zstd, gzip, deflate` if zstd also enabled; `br, zstd, gzip, deflate` if brotli enabled (brotli first due to typical superior ratio).
- Negotiation: Parses `Accept-Encoding` with q-values; chooses format with highest q (server preference breaks ties). Falls back to identity if none acceptable.
- Server preference nuance: Order in `preferredFormats` only breaks ties among encodings with equal effective q-values; encodings with strictly higher client q still win even if not listed (if you specify a subset). Listing all enabled encodings guarantees deterministic ordering.
- Threshold: `minBytes` delays compression until buffered bytes reach threshold (streaming buffers until decision). Fixed responses decide immediately.
- Streaming integration: Headers withheld until compression activation decision so `Content-Encoding` is always accurate.
- Per-response opt-out: user-supplied `Content-Encoding` (e.g. `identity`) disables automatic compression.
- `Vary: Accept-Encoding` automatically added when compression applied (configurable toggle).
- Identity safety: If threshold not met, buffered bytes flushed uncompressed with no misleading header.
- Q-value precedence: honors client preference (e.g. `gzip;q=0.1, deflate;q=0.9` chooses deflate).
- Explicit identity rejection: If `identity;q=0` and no supported positive-q encoding present -> **406 Not Acceptable** with short plain text body.

#### Zstd Tuning Example

```cpp
CompressionConfig cfg;
cfg.zstd.compressionLevel = 5;   // default ~3
cfg.zstd.windowLog = 0;          // 0 => library default; >0 to bound window explicitly
```

#### Version String Fragment

```text
aeronet 0.1.0
  tls: OpenSSL 3.0.13 30 Jan 2024
  logging: spdlog 1.15.3
  compression: zlib 1.2.13, zstd 1.5.6, brotli 1.1.0
```

#### Minimal Usage

```cpp
CompressionConfig c;
c.minBytes = 64;
c.preferredFormats = {Encoding::gzip, Encoding::deflate};
HttpServerConfig cfg; cfg.withCompression(c);
HttpServer server(cfg);
server.router().setDefault([](const HttpRequest&) {
  return HttpResponse(200, "OK").contentType("text/plain").body(std::string(1024,'A'));
});
```

Planned / future (compression): Content-Type allowlist; per-layer ratio metrics; encoder pooling; adaptive quality selection.

## Inbound Request Decompression (Config Details)

Supported: `gzip`, `deflate`, `zstd`, `br`, `identity` (skip). Order: decode reverse of header list. Safety controls:

| Field | Meaning |
|-------|---------|
| `maxCompressedBytes` | Cap on original compressed size (0 = unlimited) |
| `maxDecompressedBytes` | Cap on expanded size (0 = unlimited) |
| `maxExpansionRatio` | Per-layer `(expanded / originalTotalCompressed)` bound (0 = disabled) |

Breaches ⇒ 413. Malformed ⇒ 400. Unknown coding ⇒ 415. Disabled feature passes body through.

Example:

```cpp
RequestDecompressionConfig dc; dc.enable = true; dc.maxDecompressedBytes = 8*1024*1024;
HttpServerConfig cfg; cfg.withRequestDecompression(dc);
```

### Detailed Behavior (Inbound Decompression)

Implemented capabilities (independent from outbound compression):

| Aspect | Details |
|--------|---------|
| Supported codings | `gzip`, `deflate` when `AERONET_ENABLE_ZLIB`; `zstd` when `AERONET_ENABLE_ZSTD`; `br` when `AERONET_ENABLE_BROTLI`; `identity` always recognized |
| Multi-layer chains | Fully supported (`Content-Encoding: deflate, gzip, zstd`) decoded in reverse order (last token decoded first) |
| Parsing | Allocation-free reverse split; trims whitespace; rejects empty tokens -> **400** |
| Unknown coding | **415** (Unsupported Media Type) when feature enabled |
| Disabled feature | If `enable=false`, encodings ignored (body left compressed; no automatic 415) |
| Safety limits | `maxCompressedBytes`, `maxDecompressedBytes`, `maxExpansionRatio` guard against bombs (breach -> **413**) |
| Error mapping | Malformed data -> **400**; unknown -> **415**; ratio/size -> **413** |
| Identity in chains | Skipped (`deflate, identity, gzip`) |
| Buffering model | Aggregates full body first; decodes layer-by-layer with two alternating buffers |
| Header normalization | Removes `Content-Encoding` header after successful full decode |

Configuration:

```cpp
RequestDecompressionConfig cfg; cfg.enable = true;
cfg.maxCompressedBytes = 0;        // 0 => unlimited (still bounded by global body limit)
cfg.maxDecompressedBytes = 0;      // 0 => unlimited
cfg.maxExpansionRatio = 0.0;       // 0 => disabled ratio guard
HttpServerConfig scfg; scfg.withRequestDecompression(cfg);
```

Security / robustness notes:

- Per-layer ratio check after each stage vs original compressed size stops staged amplification.
- Absolute size guard halts decode early even if ratio guard disabled.
- Empty / whitespace-only tokens rejected early (400) to avoid ambiguous partial decode states.
- Unknown codings not skipped; fail-fast prevents partial decode inconsistencies.
- Feature off => transparent pass-through (handlers can explicitly decode if desired).

Examples:

```text
Content-Encoding: gzip                -> decode gzip
Content-Encoding: gzip, zstd          -> decode zstd then gzip
Content-Encoding: deflate, identity, gzip -> decode gzip then deflate
Content-Encoding: gzip,,deflate       -> 400
Content-Encoding: br                  -> 415 (if brotli disabled)
```

Typical handler setup:

```cpp
HttpServerConfig serverCfg; serverCfg.withRequestDecompression(RequestDecompressionConfig{});
HttpServer server(serverCfg);
server.router().setDefault([](const HttpRequest& req){
  return HttpResponse(200, "OK").body(std::string(req.body()));
});
```

## Connection Close Semantics

| Mode | Meaning | Triggers |
|------|---------|----------|
| None | Connection reusable | Normal success |
| DrainThenClose | Flush pending then close | Client `Connection: close`, keep-alive limit, explicit handler intent |
| Immediate | Abort promptly | Parse/protocol error, size breach, transport failure |

Handlers normally rely on automatic policy; unrecoverable errors escalate to Immediate.

### CloseMode Details

`ConnectionState::CloseMode` models post-response connection intent.

Helper methods:

- `requestDrainAndClose()` – escalate to `DrainThenClose` (idempotent)
- `requestImmediateClose()` – force immediate termination (used for fatal protocol / IO errors)
- `isDrainCloseRequested()`, `isImmediateCloseRequested()`, `isAnyCloseRequested()` – state queries

Behavior rationale:

- Graceful reuse preferred when protocol integrity intact.
- Immediate close chosen for malformed request lines/headers, conflicting `Content-Length` + `Transfer-Encoding`, unsupported TE, size/limit breaches (413/431), HTTP/1.0 with TE, transport failures, or internal fatal errors.
- Returning a response with `Connection: close` or exhausting `maxRequestsPerConnection` naturally transitions to `DrainThenClose`.
- Helper error paths (e.g. `emitSimpleError(..., /*immediate=*/true)`) enforce Immediate to avoid reusing a compromised parser state.

Lifecycle: parse request → build response → determine keep-alive eligibility → either mark close mode or leave connection open for next pipelined request.

## Reserved & Managed Response Headers

Managed: `Date`, `Content-Length`, `Connection`, `Transfer-Encoding`, `Trailer`, `TE`, `Upgrade`.

User attempts to override are ignored (release) / asserted (debug) except via sanctioned APIs (e.g. `contentLength`).

### Request Header Duplicate Handling (Detailed)

Incoming request headers are parsed into a flat buffer and exposed through case‑insensitive lookups on `HttpRequest`. aeronet applies a deterministic, allocation‑free in‑place policy when a duplicate request header field name is encountered while parsing. The policy is driven by a constexpr classification table that maps well‑known header names (case‑insensitive) to one of the following behaviors:

| Policy Code | Meaning | Examples |
|-------------|---------|----------|
| `,` | List merge: append a comma and the new non‑empty value | `Accept`, `Accept-Encoding`, `Via`, `Warning`, `TE` |
| `;` | Cookie merge: append a semicolon (no extra space) | `Cookie` |
| (space) | Space join: append a single space and the new non‑empty value | `User-Agent` |
| `O` | Override: keep ONLY the last occurrence (replace existing value, no concatenation) | `Authorization`, `Range`, `From`, conditional time headers |
| `\0` | Disallowed duplicate: second occurrence triggers `400 Bad Request` | `Content-Length`, `Host` |

Fallback for unknown (unclassified) headers currently assumes list semantics (`,`). This is configurable internally (a server config flag exists for future tightening) and is chosen to preserve extension / experimental headers that follow conventional `1#token` or `1#element` ABNF patterns.

Merging rules are value‑aware:

- If the existing stored value is empty and a later non‑empty value arrives, the new value replaces it (no leading separator is inserted).
- If the new value is empty and the existing value is non‑empty, no change is made (avoid trailing separators manufacturing an empty list member).
- Only when both values are non‑empty is the separator inserted (`,` / `;` / space) followed by the new bytes.
- Override (`O`) headers always adopt the last (even if empty → empty replaces previous non‑empty).

Implementation details:

1. The first occurrence of each header stores `name` and `value` as `std::string_view` slices into the connection read buffer (no copy).
2. On a mergeable duplicate, the new value bytes are temporarily copied into a scratch buffer, the tail of the original buffer is shifted right with a single `memmove`, and the separator plus new value are written into the gap. All subsequent header string_views are pointer‑adjusted (stable hashing / equality are preserved because key characters do not change, only their addresses move uniformly).
3. Override simply rebinds the existing `value` view to point at the newest occurrence (no buffer mutation).
4. Disallowed duplicates short‑circuit parsing and return `400 Bad Request` immediately.

Security / robustness notes:

- Disallowing duplicate `Content-Length` and `Host` prevents common request smuggling vectors relying on conflicting or ambiguous canonicalization across intermediaries.
- A future stricter mode may treat unknown header duplicates as disallowed instead of comma‑merging; the hook for that decision exists in the classification fallback.
- The implementation never allocates proportional to header count on a merge path; each merge performs at most one temporary copy (size of the new value) plus one tail shift.

Example:

```text
Accept: text/plain
Accept: text/html
→ Accept: text/plain,text/html
```

Summary table (quick reference):

| Policy | Action | Examples |
|--------|--------|----------|
| `,` | Comma merge non-empty | Accept, Accept-Encoding, Via |
| `;` | Semicolon merge | Cookie |
| space | Space join | User-Agent |
| `O` | Override keep last | Authorization, Range |
| disallow | 400 duplicate | Content-Length, Host |

Unknown headers default to comma merge. Empty values skipped when merging. Disallowed duplicates short‑circuit to prevent smuggling.

### Global headers

You can define global headers applied to every response of a `HttpServer` via `HttpServerConfig.globalHeaders`. These are appended after any user-set headers in a handler, so you can override them per-response if needed. Useful for consistent security headers (CSP, HSTS, etc). They will not override any header of the same name already set in a response.

Global headers are applied to every response including error responses generated internally by aeronet (400, 413, etc).

By default, it contains a `Server: aeronet` header unless you explicitly clear it out.

## Query String & Parameters

- Path percent-decoded once; invalid escape ⇒ 400.
- Query left raw; per-key/value decode on iteration (`queryParams()`).
- `+` converted to space only in query pairs.
- Missing `=` ⇒ empty value; duplicates preserved.
- Malformed escapes in query components surfaced literally (non-fatal).

Example:

```cpp
for (auto [k,v] : req.queryParams()) { /* use k,v */ }
```

## Trailing Slash Policy

`HttpServerConfig::TrailingSlashPolicy` controls how paths differing only by a single trailing `/` are treated.

Resolution algorithm:

1. Attempt an exact match first. If the incoming target exactly equals a registered path, that handler is used and the policy does not intervene.
  Note: if both `/foo` and `/foo/` were registered, they remain distinct only under the `Strict` policy. Under `Normalize` and `Redirect` the system canonicalizes paths (registrations for a trailing-slash variant are mapped to the canonical form), so duplicate registrations for the same canonical path will be merged and the first registration wins.
2. If no exact match:
   - If the request ends with a single trailing slash (excluding root `/`) and the canonical form without that slash exists:
     - Strict   – 404 (variants are distinct; no mapping)
     - Normalize – treat as the canonical path (strip the slash internally, no redirect). Note: if both `/foo` and `/foo/` were registered by the caller, only the first registration for the canonical form is kept to avoid different endpoints for normalized variants.
     - Redirect – emit `301 Moved Permanently` to the canonical path. Redirect mode operates symmetrically: if the registered canonical form has a trailing slash and the request omits it, the server will redirect to the slashed form, and vice-versa.
   - Else if the request does not end with a slash, policy is Normalize, and only the slashed variant exists (e.g. only `/foo/` registered): dispatch to that variant (symmetry in the opposite direction)
   - Otherwise: 404
3. The root path `/` is never redirected or normalized.

Behavior summary:

| Policy | `/foo` only | `/foo/` only | Both |
|--------|-------------|--------------|------|
| Strict | `/foo/`→404 | `/foo`→404 | each exact served |
| Normalize | `/foo/`→serve `/foo` | `/foo`→serve `/foo/` | only first one is registered |
| Redirect | `/foo/`→301 `/foo` | `/foo`→301 `/foo/` | only first one is registered |

Tests: `tests/http_trailing_slash.cpp`.

Usage:

```cpp
HttpServerConfig cfg; cfg.withTrailingSlashPolicy(HttpServerConfig::TrailingSlashPolicy::Redirect);
```

Rationale: Normalize avoids duplicate handler registration while preserving SEO-friendly consistent canonical paths; Redirect enforces consistent public URLs; Strict maximizes explicitness (APIs where `/v1/resource` vs `/v1/resource/` semantics differ).

## Construction Model (RAII & Ephemeral Ports)

`HttpServer` binds, configures the listening socket and registers it with epoll inside its constructor (RAII). If you request an ephemeral port (`port = 0`), the kernel-assigned port is immediately available via `server.port()` after construction (no separate setup step).

Why RAII:

- Fully initialized, listening server object or an exception (no half states)
- Simplifies lifecycle and tests (ephemeral port resolved synchronously)
- Enables immediate inspection / registration before running

Ephemeral pattern:

```cpp
HttpServerConfig cfg; // port left 0 => ephemeral
HttpServer server(cfg);
uint16_t actual = server.port();
```

Restart semantics: A single `HttpServer` is single-shot (cannot be restarted in place). Use `MultiHttpServer` for orchestrated restart cycles or construct a new instance. (Simpler, avoids re-binding races and epoll re-registration complexity.)

Removed experimental factory: a previous non-throwing `tryCreate` was dropped to keep API surface minimal.

Design trade-offs: Constructor may throw on errors (bind failure, TLS init failure if configured). This is intentional to surface unrecoverable configuration issues early.

## MultiHttpServer Lifecycle

Manages N reactors via SO_REUSEPORT.

Key points:

- Constructor binds & resolves port (ephemeral resolved once).
- Restart rebuilds underlying single‑shot servers; same port reused.
- Modify handlers only while stopped (between stop/start).
- `reusePort=true` required for `threadCount > 1`.
- Movable even while running (vector storage stable).

Example:

```cpp
HttpServerConfig cfg; cfg.port=0; cfg.reusePort=true; MultiHttpServer multi(cfg,4);
multi.router().setDefault([](const HttpRequest&){ return HttpResponse(200,"OK").contentType("text/plain").body("hi\n"); });
multi.start(); multi.stop(); multi.start();
```

## TLS Features

Optional (`AERONET_ENABLE_OPENSSL`). Provides termination, optional / required mTLS, ALPN (strict mode), handshake timeout, per‑server metrics.

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
HttpServerConfig cfg; cfg.withPort(8443)
  .withTlsCertKeyMemory(certPem, keyPem)
  .withTlsAlpnProtocols({"http/1.1"})
  .withTlsAlpnMustMatch(true)
  .withTlsMinVersion("TLS1.2")
  .withTlsMaxVersion("TLS1.3")
  .withTlsHandshakeTimeout(std::chrono::milliseconds(750));
HttpServer server(cfg);
```

Strict ALPN: if enabled and no protocol overlap, handshake aborts (connection closed, metric incremented).

### TLS (HTTPS) Support Details

TLS termination is enabled at build time with `AERONET_ENABLE_OPENSSL=ON` (default ON in main project builds). The TLS layer is isolated in a dedicated module so the core stays free of OpenSSL headers when disabled.

Key configuration helpers:

| Method | Purpose |
|--------|---------|
| `withTlsCertKey(pathCert, pathKey)` | Load certificate & key from filesystem |
| `withTlsCertKeyMemory(certPem, keyPem)` | Supply in-memory PEM strings (tests / dynamic) |
| `withTlsCipherList(list)` | Override OpenSSL cipher list (empty => library default) |
| `withTlsAlpnProtocols({..})` | Ordered ALPN protocol preference list |
| `withTlsAlpnMustMatch(true)` | Enforce overlap; abort handshake on mismatch |
| `withTlsMinVersion("TLS1.2")` / `withTlsMaxVersion("TLS1.3")` | Protocol version bounds |
| `withTlsHandshakeTimeout(ms)` | Abort slow handshakes |
| `withTlsHandshakeLogging()` | Emit per-handshake diagnostic log (cipher/version/ALPN) |
| `withTlsRequestClientCert()` | Request (but not require) client cert (mTLS optional) |
| `withTlsRequireClientCert()` | Strict mTLS (fatal if absent/invalid) |
| `withTlsAddTrustedClientCert(pem)` | Append trust anchor (repeatable) |

Client certificate modes:

- Request: server asks; absence is tolerated; presence increments stats.
- Require: absence / invalid chain => handshake termination.

ALPN behavior:

- First overlapping protocol (server order) selected; exposed via `HttpRequest::alpnProtocol`.
- Strict mode aborts if no overlap (increments mismatch counter).

Security & metrics integration:

- No global mutable OpenSSL state; each server instance owns its context to allow per-instance policies.
- Stats track: successful handshakes, strict ALPN mismatches, cert-present count, distributions (ciphers, versions, ALPN protocols), handshake duration aggregates.

Runtime notes:

- Handshake performed inside event loop with non-blocking BIO; epoll integration unchanged.
- Graceful shutdown attempts `SSL_shutdown` prior to socket close (best-effort, non-blocking).

Testing guidance:

- Use `withTlsCertKeyMemory` with ephemeral self-signed test certificates (see test helper) to avoid filesystem dependencies.
- For ALPN strict tests, provide a protocol set that intentionally does not match to exercise mismatch counter.

Roadmap (see also table above): session resumption, SNI routing, hot reload of cert/key, OCSP / revocation checks.

---

## Streaming Responses (Chunked / Incremental)

Handlers can produce bodies incrementally using a streaming handler registration instead of fixed responses. When streaming, headers are deferred until either a compression decision (if enabled) or first write.

Key semantics:

- Default transfer uses `Transfer-Encoding: chunked` unless `contentLength()` was called before any body writes.
- `write()` queues data; returns `false` only when the connection is marked to close (e.g. outbound buffer limit exceeded or fatal error). Future versions may introduce a "should-pause" state.
- `end()` finalizes, emitting terminating `0\r\n\r\n` in chunked mode and flushing any compression trailers.
- HEAD requests suppress body bytes automatically (still compute/send Content-Length when known).
- Keep-alive preserved if policy allows and no fatal condition occurred.

Backpressure & buffering:

- Unified outbound queue for both fixed & streaming; immediate write path used when queue empty, else bytes accumulate and EPOLLOUT drives flushing.
- Exceeding `maxOutboundBufferBytes` marks connection to close after pending data flush (subsequent `write()` yields false).

Limitations (current phase): no trailer support; compression integration limited to buffered activation decision; inbound streaming decompression not yet implemented.

Example:

```cpp
HttpServer server(HttpServerConfig{}.withPort(8080));
server.router().setDefault([](const HttpRequest&, HttpResponseWriter& w){
  w.setStatus(200, "OK");
  w.setHeader("Content-Type", "text/plain");
  for (int i=0;i<5;++i) {
    if (!w.write("chunk-" + std::to_string(i) + "\n")) break;
  }
  w.end();
});
```

Testing: see `tests/http_streaming.cpp`, `tests/http_streaming_keepalive.cpp`, and mixed cases in `tests/http_streaming_mixed.cpp`.

## Mixed Mode & Dispatch Precedence

Registration supports simultaneous fixed and streaming handlers at global and per-path scope. Precedence order:

1. Path-specific streaming handler
2. Path-specific fixed handler
3. Global streaming handler
4. Global fixed handler

HEAD requests fallback to GET semantics for handler selection; streaming handlers auto-suppress body for HEAD.

Conflict rules:

- Registering both streaming & fixed for the identical (path, method) pair is rejected.
- Distinct method sets on same path may split across streaming vs fixed registrations (e.g. GET streaming, POST fixed).

Example precedence illustration:

```cpp
server.router().setDefault([](const HttpRequest&){ return HttpResponse(200,"OK").body("GLOBAL").contentType("text/plain"); });
server.router().setDefault([](const HttpRequest&, HttpResponseWriter& w){ w.setStatus(200,"OK"); w.setHeader("Content-Type","text/plain"); w.write("STREAMFALLBACK"); w.end(); });
server.router().setPath("/stream", http::Method::GET, [](const HttpRequest&, HttpResponseWriter& w){ w.setStatus(200,"OK"); w.setHeader("Content-Type","text/plain"); w.write("PS"); w.end(); });
server.router().setPath("/stream", http::Method::POST, [](const HttpRequest&){ return HttpResponse{201, "Created", "text/plain", "NORMAL"}; });
```

Behavior:

- GET /stream → path streaming
- POST /stream → path fixed
- GET /other → global streaming fallback
- POST /other → global fixed (since only global fixed + streaming; precedence chooses streaming for GET only)

Testing: `tests/http_streaming_mixed.cpp` covers precedence, conflicts, HEAD suppression, keep-alive reuse.

### Accessing TLS Metrics

```cpp
auto st = server.stats();
std::print("handshakes={} clientCerts={} alpnStrictMismatches={}\n",
           st.tlsHandshakesSucceeded,
           st.tlsClientCertPresent,
           st.tlsAlpnStrictMismatches);
for (auto& [proto,count] : st.tlsAlpnDistribution) {
  std::print("ALPN {} -> {}\n", proto, count);
}
```

Metric fields include: handshake success/fail counts, strict ALPN mismatches, distribution of ALPN protocols, TLS versions, ciphers, handshake duration aggregate (count / total / max).

Security note: No process‑global mutable TLS state; each server instance tracks metrics independently.

Test usage: In-memory PEM configuration is convenient for ephemeral test cert generation.

Failure modes: missing key/cert, invalid PEM, unsupported protocol versions (outside bounds), ALPN mismatch under strict mode.

## Logging

If built with `AERONET_ENABLE_SPDLOG`, aeronet uses spdlog sinks/formatting; otherwise a lightweight fallback replicates the API (`log::info("msg {}", v)`). Fallback uses `std::vformat` when available; failures degrade gracefully by concatenating arguments.

Characteristics:

- ISO 8601 UTC timestamps (ms precision)
- Levels: trace, debug, info, warn, error, critical
- Runtime level adjustable: `aeronet::log::set_level(aeronet::log::level::debug);`
- Dependency-free by default (flags opt-in to spdlog)
- Planned: pluggable structured sinks / user-defined writer API

Design goals: keep logging off the hot path when disabled, avoid mandatory third-party dependency for minimal builds, allow future structured logging integration without breaking existing code.

Usage example (fallback or spdlog):

```cpp
aeronet::log::info("server listening on {}", server.port());
```

## OpenTelemetry Integration

Optional (`AERONET_ENABLE_OPENTELEMETRY`). Provides distributed tracing and metrics via OpenTelemetry SDK.

### Architecture

**Instance-based telemetry.** Each `HttpServer` owns its own `TelemetryContext` instance. No global singletons or static state.

Key design principles:

- Per-instance isolation: Multiple servers with independent telemetry configurations
- Explicit lifecycle: Telemetry instance tied to server lifetime
- Error transparency: All telemetry failures logged via `log::error()` (no silent no-ops)

Configuration via `HttpServerConfig::otel`:

```cpp
HttpServerConfig cfg;
cfg.withOtelConfig(OtelConfig{
  .enabled = true,
  .endpoint = "http://localhost:4318",  // OTLP HTTP endpoint base URL
  .serviceName = "my-service",
  .sampleRate = 1.0  // trace sampling rate (0.0 to 1.0)
});
```

### Built-in Instrumentation (phase 1)

Automatic (no handler code changes):

**Traces:**

- `http.request` spans for each HTTP request
- Attributes: `http.method`, `http.target`, `http.status_code`, `http.request.body.size`, `http.response.body.size`

**Metrics (counters):**

- `aeronet.events.processed` – epoll events processed
- `aeronet.connections.accepted` – new connections
- `aeronet.bytes.read` – bytes read from clients
- `aeronet.bytes.written` – bytes written to clients

All instrumentation is fully async (OTLP HTTP exporter) with configurable endpoints and sample rates.

### Testing & Observability

Comprehensive integration tests validate:

- Multi-instance contexts with independent configurations
- Span creation, attribute setting, lifecycle
- Counter operations under various conditions
- Error handling and logging

Use with OpenTelemetry Collector for full observability pipeline:

```yaml
# Example collector config for testing
receivers:
  otlp:
    protocols:
      http:
        endpoint: 0.0.0.0:4318

exporters:
  logging:
    loglevel: debug

service:
  pipelines:
    traces:
      receivers: [otlp]
      exporters: [logging]
    metrics:
      receivers: [otlp]
      exporters: [logging]
```

---

## Future Expansions

Planned / potential: trailers, streaming inbound decompression, encoder pooling, compression ratio metrics, TLS hot reload & SNI, richer logging & metrics, additional OpenTelemetry instrumentation (histograms, gauges).
