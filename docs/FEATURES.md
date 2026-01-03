# aeronet Feature Reference

Single consolidated reference for **aeronet** features.

## Index

1. [HTTP/1.1 Feature Matrix](#http11-feature-matrix)
1. [Performance / architecture](#performance--architecture)
1. [Compression & Negotiation](#compression--negotiation)
1. [Inbound Request Decompression (Config Details)](#inbound-request-decompression-config-details)
1. [Chunked Transfer Encoding (RFC 7230 §4.1)](#chunked-transfer-encoding-rfc-7230-41)
1. [Connection Close Semantics](#connection-close-semantics) — includes graceful drain lifecycle
1. [Reserved & Managed Response Headers](#reserved--managed-response-headers)
1. [Request Header Duplicate Handling (Detailed)](#request-header-duplicate-handling-detailed)
1. [Path Handling](#path-handling)
1. [Middleware Pipeline](#middleware-pipeline)
1. [Trailing Slash Policy](#trailing-slash-policy)
1. [Construction Model (RAII & Ephemeral Ports)](#construction-model-raii--ephemeral-ports)
1. [HttpServer Lifecycle](#httpserver-lifecycle)
1. [Built-in Kubernetes-style probes](#built-in-kubernetes-style-probes)
1. [TLS Features](#tls-features)
1. [CONNECT (HTTP tunneling)](#connect-http-tunneling)
1. [Streaming Responses](#streaming-responses-chunked--incremental)
1. [Static File Handler (RFC 7233 / RFC 7232)](#static-file-handler-rfc-7233--rfc-7232)
1. [Mixed Mode Dispatch Precedence](#mixed-mode--dispatch-precedence)
1. [Logging](#logging)
1. [OpenTelemetry Integration](#opentelemetry-integration)
1. [Access-Control (CORS) Helpers](#access-control-cors-helpers)
1. [WebSocket (RFC 6455)](#websocket-rfc-6455)
1. [HTTP/2 (RFC 9113)](#http2-rfc-9113)
1. [Future Expansions](#future-expansions)
1. [Large-body optimization](#large-body-optimization)

## HTTP/1.1 Feature Matrix

Legend: [x] implemented, [ ] planned / not yet.

### Core HTTP parsing & routing

- [x] Request line parsing (method, target, version)
- [x] Header field parsing (no folding / continuations)
- [x] Case-insensitive header lookup helper
- [x] Router path matching and allowed-method computation
- [x] Method token parsing / matching is case-insensitive (incoming method tokens like `GET`, `get`, `GeT` are accepted and normalized)
- [x] Pipelined sequential requests (no parallel handler execution)

Where to look: see the "Core parsing & connection handling" and router sections below for details.

### Transport & connection

- [x] Persistent connections (HTTP/1.1 default, HTTP/1.0 opt-in)
- [x] HTTP/1.0 response version preserved (no silent upgrade)
- [x] Connection: close handling
- [x] CONNECT tunneling (proxy-style TCP CONNECT handling)
- [x] Backpressure / partial write buffering
- [x] Header read timeout (Slowloris mitigation) (configurable, disabled by default)
- [x] Keep-alive limits (maxRequestsPerConnection)

Where to look: see the "CONNECT (HTTP tunneling)" subsection and the Connection Manager notes for implementation details.

### Request bodies & decoding

- [x] Content-Length bodies with size limit
- [x] Chunked Transfer-Encoding decoding (request) with trailer header support (RFC 7230 §4.1.2)
- [x] Trailer header exposure (incoming chunked trailers)
- [x] Outbound trailer headers (response trailers for both buffered and streaming responses)
- [x] Content-Encoding request body decompression (gzip, deflate, zstd, multi-layer, identity skip, safety limits)
- [x] Multipart/form-data convenience utilities
- [x] Forbidden trailer headers rejected for incoming chunked trailers (security)

Where to look: see "Inbound Request Decompression (Config Details)" for decompression behavior and the parser docs for chunked/CL handling.

### Response generation & streaming

- [x] Basic fixed body responses
- [x] HEAD method (suppressed body, correct Content-Length)
- [x] Outgoing chunked / streaming responses (basic API: status/headers + incremental write + end, keep-alive capable)
- [x] Outbound trailer headers (buffered via HttpResponse::addTrailer, streaming via HttpResponseWriter::addTrailer)
- [x] Mixed-mode dispatch (simultaneous registration of streaming and fixed handlers with precedence)
- [x] Compression (gzip & deflate) (phase 1: zlib) – streaming + buffered with threshold & q-values
- [x] Large-body optimization (zero-copy capture for large fixed responses)
- [x] Identity rejection -> 406 Not Acceptable when `identity;q=0` and no acceptable encoding

Where to look: see the "Compression & Negotiation" section for full details and configuration.

### Methods & special semantics

- [x] OPTIONS * handling (returns an Allow header per RFC 7231 §4.3)
- [x] TRACE method support (echo) — optional and configurable via `HttpServerConfig::TracePolicy`
- [x] CONNECT method support — proxy-style TCP tunneling to an upstream host:port target.

Where to look: see the "OPTIONS & TRACE behavior" subsection below.

### Status & error handling

- [x] 400 Bad Request (parse errors, CL+TE conflict)
- [x] 400 on HTTP/1.0 requests carrying Transfer-Encoding
- [x] 405 Method Not Allowed (enforced when path exists but method not in allow set)
- [x] 406 Not Acceptable (identity rejected when no acceptable Accept-Encoding)
- [x] 413 Payload Too Large (body limit)
- [x] 415 Unsupported Media Type (content-encoding based)
- [ ] 415 Unsupported Media Type (content-type based)
- [x] 417 Expectation Failed (unknown `Expect` token when no handler installed)
- [x] 431 Request Header Fields Too Large (header limit)
- [x] 500 Internal Server Error (invalid interim status returned by ExpectationHandler for instance)
- [x] 501 Not Implemented (unsupported Transfer-Encoding)
- [x] 505 HTTP Version Not Supported
  
  Note: aeronet already maps unknown request `Content-Encoding` values to **415** when the inbound
  decompression feature is enabled (see "Inbound Request Decompression"). However, automatic
  `Content-Type` (media-type) validation is intentionally left to application code. If you need
  global Content-Type enforcement, implement a small validator middleware or configure your handlers
  to check the `Content-Type` header and return **415** when appropriate.

Where to look: see the "Status & error handling" notes and parser error descriptions below.

### Headers & protocol niceties

- [x] Request header duplicate handling (merge/override/disallow policies)
- [ ] Optional stricter duplicate-header policy (fail on unknown duplicates)
TRACE semantics and safety:

- TRACE, when allowed, echoes the received request (start-line, headers and body) back with `Content-Type: message/http` so it can be used for debugging loopback-style probes as per RFC 7231 §4.3.2.
- The server exposes a `TracePolicy` in `HttpServerConfig` with the following values:
  - `Disabled` — TRACE disallowed (default).
  - `Enabled` — TRACE allowed on both plaintext and TLS connections.
  - `EnabledPlainOnly` — TRACE allowed on plaintext connections only; rejected on TLS.

Server enforcement uses the per-request TLS indicator (e.g. `HttpRequest::tlsVersion()` being non-empty for TLS) to make the decision when `TracePolicy` is one of the TLS-bound options. For backward compatibility the config also provides `withEnableTrace(bool)` as a convenience that maps `true` to `Enabled` and `false` to `Disabled`.

Use cases:

- If you deploy behind TLS-terminating proxies and want to avoid exposing TRACE responses over TLS endpoints, set `TracePolicy::EnabledPlainOnly`.
  
Note: `EnabledOnTls` (TRACE allowed only on TLS) was removed — the policy set is now intentionally smaller and focuses
on disabling TRACE entirely, allowing it everywhere, or allowing it only on plaintext.

### Expect header handling (RFC 7231 §5.1.1)

- [x] `Expect` header processing with opt-in application-level expectation handler

Behavior summary

- The server preserves the standard `100-continue` semantics: when a client sends `Expect: 100-continue` the server
  will emit `100 Continue` if the request proceeds to body reading. Detection recognizes `100-continue` even when
  it appears in a comma-separated `Expect` header list and tolerates surrounding whitespace per the RFC.
- For expectation tokens other than `100-continue` aeronet exposes an opt-in `ExpectationHandler` API (see
  `SingleHttpServer::setExpectationHandler` in `http-server.hpp`). When present, the handler is invoked with the
  parsed expectation token and may respond with one of:
  - Continue — allow normal request processing
  - Interim — emit an informational 1xx interim response (handler supplies the specific 1xx status)
  - FinalResponse — send the supplied final response immediately and abort normal request processing
  - Reject — equivalent to `417 Expectation Failed` (server will send 417)
- Default behavior when no handler is installed: any non-`100-continue` expectation token is treated as unknown and the
  server responds with **417 Expectation Failed** per the RFC.

Implementation notes & constraints

- The handler is invoked on the server's event-loop thread and must be fast; heavy work should be deferred to worker
  threads by the application.
- If the handler returns an `Interim` result its `interimStatus` MUST be an informational status in the 1xx range;
  an invalid interim status is treated as a server bug and the server will log an error and return **500 Internal Server
  Error** for that request (the request body will not be processed). This validation prevents sending non-1xx interim
  responses.
- The Expect parsing and handler dispatch is implemented in `SingleHttpServer::handleExpectHeader(...)` (internal helper).
- See unit tests in `tests/http_additional_test.cpp` for example usages and behavior expectations (including mixed
  `Expect` lists containing `100-continue` and custom tokens).

### CONNECT (HTTP tunneling)

- [x] CONNECT method support — proxy-style TCP tunneling to an upstream host:port target.

Behavior summary

- On receiving a `CONNECT host:port HTTP/1.1` request the server attempts to resolve the target and establish a
  non-blocking TCP connection to the upstream address. If the connect attempt succeeds (or is in progress), the server
  replies `200 Connection Established` and links the client and upstream sockets into a tunneling pair. From that
  point the connections bypass HTTP parsing and are proxied bidirectionally until either side closes.
- The server uses a small `ConnectResult` helper to capture whether the upstream connection completed immediately or is
  still pending (`EINPROGRESS`) on a non-blocking socket. Pending connects are tracked using a `connectPending` flag
  on the upstream `ConnectionState`; when the event loop notifies writable readiness we check `SO_ERROR` to determine
  whether the connect completed successfully or failed and, on failure, attempt to notify the client with `502`.
- For tunneling we record `peerFd` on each side (client and upstream). A connection is considered in tunneling mode when
  `peerFd != -1` (exposed via `ConnectionState::isTunneling()` accessor). When the tunnel is active bytes read on one
  side are written to the peer's transport directly. Each side keeps a dedicated tunnel buffer for the peer flow so
  frontend HTTP outbound buffering (`outBuffer`) and tunnel forwarding remain separate.

Configuration

- `HttpServerConfig::connectAllowlist` — optional list of allowed target hosts (exact string match). When empty, any
  resolved host is allowed. To populate conveniently use `withConnectAllowlist()` builder helper.

Notes and implementation details

- The CONNECT implementation carefully handles container rehashing: inserting the upstream connection into the server's
  internal `_connStates` map may rehash and invalidate iterators. To avoid UB the insertion re-resolves the client
  iterator (and updates the caller's iterator when appropriate).
- The tunneling path prioritizes a dedicated `tunnelOutBuffer` to avoid mixing HTTP response buffering semantics with
  raw tunneled bytes. This keeps the HTTP response life-cycle and the TCP proxying semantics independent and easier to
  reason about.
- Tests: Basic coverage added for successful echo tunneling and failure cases (DNS resolution failures and allowlist
  rejections). See `tests/http_connect_test.cpp`.

## Performance / architecture

### Execution model & scaling

- [x] Single-thread event loop (one server instance)
- [x] Horizontal scaling via SO_REUSEPORT (multi-reactor)
- [x] Multi-instance orchestration wrapper (`HttpServer` aka `MultiHttpServer`) (forces `reusePort=true` for >1 threads; aggregated stats; resolved port immediately after construction)
- [x] writev scatter-gather for response header + body
- [x] Outbound write buffering with EPOLLOUT-driven backpressure
- [x] Header read timeout (Slowloris mitigation) (configurable, disabled by default)
- [ ] Benchmarks & profiling docs
- [x] Zero-copy sendfile() support for static files

### Safety / robustness

- [x] Configurable header/body limits
- [x] Graceful shutdown loop (runUntil)
- [x] Slowloris style header timeout mitigation (implemented as header read timeout)
- [x] TLS termination (OpenSSL) with ALPN, mTLS, version bounds, handshake timeout & per-server metrics
- [x] Graceful drain lifecycle (beginDrain / stop semantics)

### Developer experience

- [x] Server objects moveable
- [x] Builder style HttpServerConfig
- Note: Some configuration fields are immutable after construction (for example: `port`, `reusePort`, and OpenTelemetry setup). Most mutable fields (limits, timeouts, compression, headers, TLS configuration) are runtime-updatable via `postConfigUpdate()`. See `docs/CONFIG_MUTABILITY.md` for the full field-by-field guide.
- [x] Simple lambda handler signature
- [x] Simple exact-match per-path routing (`setPath`)
- [x] Configurable trailing slash handling (Strict / Normalize / Redirect)
- [x] Lightweight built-in logging (spdlog optional integration) – pluggable interface TBD
- [x] Built-in Kubernetes-style probes (liveness/readiness/startup)
- [x] OpenTelemetry integration (optional build flag)
- [x] Middleware helpers (global + per-route pre/post chains, streaming support)
- [ ] Pluggable logging interface (abstract sink / formatting hooks)
- [x] Ephemeral port support (server.port() available after construction)
- [x] JSON stats export / per-server metrics
- [ ] Pluggable structured sinks / user-defined writer API

## Large body optimization

To improve performance when serving large fixed responses (for example, generated payloads or read-in files),
**aeronet** implements a large-body optimization that may save one copy (and growing allocation) by capturing the body by value.
This section explains the behavior and the supported capture types.

### Key points

- Some `HttpResponse::body(...)` overloads accept non-owning views such as `const char*` or `std::string_view`.
  These overloads copy the referenced bytes into the response's inline/buffered storage and therefore force an
  allocation + copy even for large inputs. To avoid that allocation for large buffers, prefer the move-friendly
  overloads shown above (`std::string`, `std::vector<char>`, `std::unique_ptr<char[]>`) which hand ownership to the
  server without copying.
- Currently only the non-streaming `HttpResponse` API is affected (streaming responses always write from user buffers).
  The streaming `HttpResponseWriter` only partially supports this optimization internally.
- When a handler returns an `HttpResponse` with a body whose size is lower or equal to a configurable threshold
  (`HttpServerConfig::minCapturedBodySize`), the captured body will be appended inline with the response head.

### Ergonomic body capture types

The `HttpResponse` body API accepts several convenient ownership types so handlers may hand off buffers without
copying:

- `std::string` — move a string into the response body for zero-copy handoff;
- `std::vector<char>` — move a character vector when your data is in a non-null-terminated buffer;
- `std::unique_ptr<char[]>` — for blob ownership without a resizing container (move-only `unique_ptr` semantics).

### Usage examples

It is possible to avoid a full allocation + copy for large buffers by moving ownership of an existing buffer into
the response. The `HttpResponse` API accepts move-only types and will take ownership, so the server does not need to
allocate a new buffer and copy bytes.

Short examples:

```cpp
// Move a std::string into the response
std::string big /* = generate_large_string() */;
HttpResponse(200, "OK").body(std::move(big), "application/octet-stream");

// Move a vector<char>
std::vector<char> v /* = read_file_bytes(path) */;
HttpResponse(200, "OK").body(std::move(v), "application/octet-stream");

// Move a unique_ptr<char[]> for raw blob ownership
std::unique_ptr<char[]> blob /* = load_blob() */;
std::size_t blobSize /* = known size */;
HttpResponse(200, "OK").body(std::move(blob), blobSize, "application/octet-stream");
```

These patterns hand ownership to the server without duplicating the payload, enabling efficient zero-copy handoff
for large responses.

### Appending body data

The `HttpResponse::appendBody(...)` overloads allows appending additional data to an existing body.

For maximum efficiency, use the overloads taking a `writer` lambda to write directly into the response's internal
buffer without intermediate copies.

Example:

```cpp
HttpResponse resp(200, "OK");

// Append a simple string line
resp.appendBody("Header line\n");

// Append generated data via writer lambda for maximum efficiency
std::size_t maxLen = 256;
resp.appendBody(maxLen, [](char* buf) -> std::size_t {
  // write directly into buf up to bufSize bytes
  std::string_view data = "Body data generated on the fly...\n";
  std::memcpy(buf, data.data(), data.size());
  return data.size(); // return number of bytes actually written (should be less than maxLen)
});
```

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
However, if you explicitly set a `Content-Encoding` header yourself (via `header()` / `contentEncoding()` or on a
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
std::string preCompressedHelloGzipBytes /* = gzip-compressed "Hello, World!" */;
Router router;
router.setDefault([&](const HttpRequest&, HttpResponseWriter& w){
  w.status(http::StatusCodeOK);
  w.contentType(http::ContentTypeTextPlain);
  w.contentEncoding("gzip");            // suppress auto compression
  w.writeBody(preCompressedHelloGzipBytes);  // already gzip-compressed data
  w.end();
});
```

To “force identity” even if thresholds would normally trigger compression:

```cpp
std::string largePlainBuffer(10 * 1024 * 1024, 'A'); // 10 MiB of 'A's
Router router;
router.setDefault([&](const HttpRequest&, HttpResponseWriter& w){
  w.contentEncoding("identity"); // blocks auto compression
  w.writeBody(largePlainBuffer);
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

Router router;
router.setDefault([](const HttpRequest&) {
  return HttpResponse(200, "OK").body(std::string(1024,'A'));
});

SingleHttpServer server(cfg, std::move(router));
```

## Multipart/form-data utilities (RFC 7578)

`MultipartFormData` parses aggregated `multipart/form-data`
payloads with zero-copy `std::string_view` slices referencing the original request buffer. Use it after calling
`req.body()` / `co_await req.bodyAwaitable()` so the full payload is buffered.

### Basic usage

```cpp
Router router;
router.setPath(http::Method::POST, "/upload", [](const HttpRequest& req) {
  const auto body = req.body();
  const auto contentType = req.headerValueOrEmpty(http::ContentType);

  MultipartFormData form(contentType, body);
  if (!form.valid()) {
    std::string body("invalid multipart payload: ");
    body += form.invalidReason();
    return HttpResponse(400).body(std::move(body));
  }

  if (const auto* note = form.part("description")) {
    log::info("desc={} bytes", note->value.size());
  }

  if (const auto* file = form.part("file")) {
    if (file->filename) {
      // user helper
      // PersistFile(*file->filename, file->value);
    }
  }

  return HttpResponse(204);
});
```

Each `Part` exposes `name`, optional `filename`/`contentType`, the raw `value`, and a span of headers (use
`headers()` or `headerValueOrEmpty()`). `part("field")` returns the first match, while `parts("field")` gathers duplicates.

### Options & limits

`MultipartFormDataOptions` protects against abusive payloads:

| Option | Default | Effect |
|--------|---------|--------|
| `maxParts` | 128 | Rejects payloads containing more than this many parts (0 disables the check). |
| `maxHeadersPerPart` | 32 | Caps the number of header lines per part. |
| `maxPartSizeBytes` | 32 MiB | Rejects an individual part when its body would exceed this size (0 disables the check). |

Malformed payloads (missing boundary markers, absent `Content-Disposition`, exceeded limits) leave
`MultipartFormData::valid()` set to `false` and produce no parts. The parser never throws for content/limit issues, so
handlers should check `valid()` and return an appropriate 4xx response (or log/recover) when it is `false`.

Current behavior:

- Accepts quoted boundary attributes (`boundary="Aa--123"`).
- Understands the simple `filename*=` syntax (RFC 5987) by returning the substring after the second `'` (percent decoding
  can be layered on top if needed).
- Requires CRLF-delimited MIME boundaries (per RFC 7578).
- Optimized for aggregated bodies; streaming multipart parsing is a future roadmap item.

## Inbound Request Decompression (Config Details)

Supported: `gzip`, `deflate`, `zstd`, `br`, `identity` (skip). Order: decode reverse of header list. Safety controls:

| Field | Meaning |
|-------|---------|
| `maxCompressedBytes` | Cap on original compressed size (0 = unlimited) |
| `maxDecompressedBytes` | Cap on expanded size (0 = unlimited) |
| `maxExpansionRatio` | Per-layer `(expanded / originalTotalCompressed)` bound (0 = disabled) |
| `streamingDecompressionThresholdBytes` | Enable streaming inflaters when `Content-Length >= threshold` (0 = disabled) |

Breaches ⇒ 413. Malformed ⇒ 400. Unknown coding ⇒ 415. Disabled feature passes body through.

Example:

```cpp
DecompressionConfig dc;
dc.enable = true;
dc.maxDecompressedBytes = 8*1024*1024;
HttpServerConfig cfg;
cfg.withRequestDecompression(dc);
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
| Buffering model | Aggregates full body first; optional streaming inflaters kick in automatically when the Content-Length crosses the configured threshold |
| Header normalization | Removes `Content-Encoding` header after successful full decode |

Configuration:

```cpp
DecompressionConfig cfg; cfg.enable = true;
cfg.maxCompressedBytes = 0;        // 0 => unlimited (still bounded by global body limit)
cfg.maxDecompressedBytes = 0;      // 0 => unlimited
cfg.maxExpansionRatio = 0.0;       // 0 => disabled ratio guard
cfg.streamingDecompressionThresholdBytes = 512 * 1024;  // switch to streaming inflaters when CL >= 512 KiB
HttpServerConfig scfg; scfg.withRequestDecompression(cfg);
```

When `streamingDecompressionThresholdBytes` is non-zero, aeronet automatically routes large encoded payloads through
streaming decoder contexts (per codec) instead of materializing every intermediate stage at once. Each stage consumes the
compressed data in `decoderChunkSize` slices and appends the decoded bytes to the alternating buffers already used for
the aggregated path, so handlers still see a single contiguous `req.body()`.

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
HttpServerConfig serverCfg; serverCfg.withRequestDecompression(DecompressionConfig{});
Router router;
router.setDefault([](const HttpRequest& req){
  return HttpResponse(200, "OK").body(std::string(req.body()));
});
SingleHttpServer server(std::move(serverCfg), std::move(router));
```

Streaming example (switch to inflaters when compressed payloads reach 1 MiB):

```cpp
DecompressionConfig big;
big.streamingDecompressionThresholdBytes = 1024 * 1024;
big.decoderChunkSize = 32 * 1024;  // keep each streaming slice manageable
HttpServerConfig cfg; cfg.withRequestDecompression(big);
```

## Chunked Transfer Encoding (RFC 7230 §4.1)

**aeronet** implements full support for chunked transfer encoding on incoming HTTP/1.1 requests, including chunk extensions (§4.1.1), trailer headers (§4.1.2), and decoding chunked (§4.1.3) as specified in RFC 7230.

### Chunked Encoding Format

Chunked format consists of:

1. **Chunk size line**: hex size (without 0x prefix), optional chunk extensions (after semicolon), CRLF
2. **Chunk data**: `size` bytes of actual data
3. **Chunk ending**: CRLF
4. **Zero chunk**: `0` followed by optional trailer headers
5. **Final CRLF**: Terminating the message

Example:

```http
POST /upload HTTP/1.1
Host: example.com
Transfer-Encoding: chunked

4
Wiki
5
pedia
0
X-Checksum: abc123
X-Timestamp: 2025-10-20T12:00:00Z

```

### Chunk Extensions (RFC 7230 §4.1.1)

Chunk extensions allow metadata to be attached to individual chunks via semicolon-separated parameters after the chunk size:

```text
<size-hex>;<extension-name>[=<extension-value>]
```

**aeronet behavior**: Chunk extensions are **parsed and silently ignored**. The parser validates their syntax (presence of semicolon) but does not expose or process the extension data. This follows the RFC's guidance that chunk extensions are primarily for protocol extensions and should not affect basic message processing.

Example (extension ignored but accepted):

```text
7;metadata=test
payload
```

### Trailer Headers (RFC 7230 §4.1.2)

Trailers are HTTP headers that appear after the final zero-size chunk. They allow metadata to be sent after the body (useful for checksums, signatures, or other computed values).

**aeronet behavior**: Trailer headers are **fully supported**. Trailers are:

- Parsed from the chunk stream after the `0\r\n` terminator
- Exposed via `HttpRequest::trailers()` (case-insensitive map)
- Subject to the same size limit as regular headers (`maxHeaderBytes`)
- Validated for forbidden headers (security-sensitive headers cannot appear as trailers)

**Forbidden trailer headers** (per RFC 7230 §4.1.2 and security best practices):

- Authentication & authorization: `Authorization`, `Proxy-Authorization`, `Proxy-Authenticate`, `WWW-Authenticate`
- Content framing: `Transfer-Encoding`, `Content-Length`, `Content-Range`, `Content-Encoding`, `Content-Type`
- Request control: `Host`, `Cache-Control`, `Expect`, `Max-Forwards`, `Pragma`, `Range`, `TE`
- Metadata: `Trailer`, `Set-Cookie`, `Cookie`

Attempting to send forbidden headers as trailers results in **400 Bad Request**.

#### Trailer API

```cpp
Router router;
router.setPath(http::Method::GET, "/upload", [](const HttpRequest& req) {
  // Access request body
  std::string_view body = req.body();
  
  // Access trailer headers (if any)
  auto checksum = req.trailers().find("X-Checksum");
  if (checksum != req.trailers().end()) {
    std::string checksumValue = std::string(checksum->second);
    // Validate checksum against body...
  }
  
  return HttpResponse(200).body("OK");
});
```

**Memory optimization**: Trailers are stored in the same connection buffer as the body data (`bodyAndTrailersBuffer`), with a `trailerStartPos` marker indicating where trailer data begins. This avoids additional allocations and maintains zero-copy string_view semantics.

### Decoding Chunked (RFC 7230 §4.1.3)

aeronet's chunked decoder implements the full decoding algorithm specified in §4.1.3:

1. **Parse chunk size**: Read hex digits until CRLF or semicolon (chunk extension marker)
2. **Handle chunk extensions**: If semicolon found, skip to CRLF (extensions ignored)
3. **Read chunk data**: Copy `size` bytes into body buffer
4. **Consume chunk CRLF**: Validate and skip the trailing CRLF
5. **Repeat** until zero-size chunk encountered
6. **Parse trailers**: After `0\r\n`, parse optional trailer headers
7. **Consume final CRLF**: Validate blank line terminating the message

**Error handling**:

- Invalid hex digits → **400 Bad Request**
- Missing CRLF → need more data (or **400** if size limit reached)
- Chunk data exceeds `maxBodyBytes` → **413 Payload Too Large**
- Malformed trailers (no colon, forbidden headers) → **400 Bad Request**
- Trailer section exceeds `maxHeaderBytes` → **431 Request Header Fields Too Large**

**Integration with other features**:

- Chunked decoding happens **before** Content-Encoding decompression
- The complete, decoded body is available via `HttpRequest::body()`
- Trailers are available via `HttpRequest::trailers()` after the request is fully parsed
- `CONNECT` tunneling bypasses chunked decoding (raw TCP proxy mode)

### Implementation Notes

- **State machine**: Chunked decoding is implemented as part of the main HTTP parser state machine in `http-parser.cpp`
- **Buffer management**: Chunk data is appended to `bodyAndTrailersBuffer` as chunks are decoded; trailer text is appended after the final chunk with `trailerStartPos` marking the boundary
- **Zero-copy trailers**: Trailer name/value pairs are stored as `string_view` references into `bodyAndTrailersBuffer`, avoiding string copies
- **Whitespace trimming**: Trailer values have leading/trailing whitespace (OWS per RFC 7230 §3.2) automatically trimmed
- **Case-insensitive trailer lookup**: Trailer map uses the same case-insensitive hash/equality comparator as regular headers

### Configuration

Chunked encoding behavior is controlled by existing size limits:

```cpp
HttpServerConfig cfg;
cfg.maxBodyBytes = 16 * 1024 * 1024;     // Limit total decoded body size
cfg.maxHeaderBytes = 8 * 1024;           // Limit trailer header section size
```

**Security considerations**:

- Trailer size is bounded by `maxHeaderBytes` to prevent trailer header bombs
- Total body (all decoded chunks) is bounded by `maxBodyBytes`
- Forbidden trailer headers are rejected to prevent request smuggling attacks
- Chunk extensions are parsed but ignored to avoid complexity attacks

### Outbound Trailers (Response Trailers)

aeronet supports sending HTTP trailers in responses, allowing metadata to be transmitted after the response body. This is useful for checksums, signatures, or other values computed while streaming the response.

**Two APIs for different response patterns**:

1. **Buffered responses** (`HttpResponse`): Trailers added via `addTrailer()` after body is set
2. **Streaming responses** (`HttpResponseWriter`): Trailers added during streaming, emitted in final chunk

#### Buffered Response Trailers (HttpResponse)

For fixed/buffered responses, use `HttpResponse::addTrailer()`:

```cpp
Router router;
router.setPath(http::Method::GET, "/data", [](const HttpRequest& req) {
  HttpResponse resp(200);
  resp.body("response data");
  
  // Add trailers after body (required)
  resp.addTrailer("X-Checksum", "abc123");
  resp.addTrailer("X-Timestamp", "2025-10-20T12:00:00Z");
  
  return resp;
});
```

**Constraints**:

- **Trailers MUST be added AFTER the body** is set (via `body()` or `bodyOwned()`)
- Attempting to add trailers before the body throws `std::logic_error`
- This ensures correct ordering in the final serialized response

**Zero-allocation design**:

- Trailers are **appended directly** to the existing body buffer (no separate allocation)
- For inline bodies: appended to the single buffer
- For captured bodies: appended to captured body buffer
- Format: `name: value\r\n` for each trailer, terminated with `\r\n`

**Method chaining**:

```cpp
HttpResponse(200)
    .header("X-Custom", "value")
    .body("data")
    .addTrailer("X-Checksum", "xyz")
    .addTrailer("X-Signature", "sig123");
```

#### Streaming Response Trailers (HttpResponseWriter)

For chunked/streaming responses, use `HttpResponseWriter::addTrailer()`:

```cpp
Router router;
router.setPath(http::Method::GET, "/stream",
    [](const HttpRequest& req, HttpResponseWriter& w) {
  w.status(200);
  w.writeBody("chunk1");
  w.writeBody("chunk2");
  
  // Add trailers during streaming
  w.addTrailer("X-Checksum", "computed-hash");
  w.addTrailer("X-Row-Count", "12345");
  
  w.end();  // Trailers emitted in final chunk
});
```

**Behavior**:

- Trailers are **buffered internally** and emitted when `end()` is called
- Only supported for **chunked responses** (Transfer-Encoding: chunked)
- If `contentLength()` was set (fixed-length response), trailers are **silently ignored** with a warning log
- Trailers added after `end()` are also ignored with a warning

**Wire format** (RFC 7230 §4.1.2):

```text
HTTP/1.1 200 OK
Transfer-Encoding: chunked

6\r\n
chunk1\r\n
6\r\n
chunk2\r\n
0\r\n
X-Checksum: computed-hash\r\n
X-Row-Count: 12345\r\n
\r\n
```

The final `0\r\n` is the zero-length chunk indicating end of body, followed by trailer lines and a blank line.

**Memory management**:

- Trailers are buffered in their own buffer
- Buffer size is reserved upfront when emitting the final chunk
- Final chunk string is moved into HttpPayload for efficient transmission

#### Trailer Validation

**Application responsibility**: aeronet does **not** validate trailer names against the forbidden list when sending responses (for performance). Applications should avoid sending:

- Content-framing headers: `Transfer-Encoding`, `Content-Length`, `Content-Encoding`, `Content-Type`
- Authentication headers: `Authorization`, `WWW-Authenticate`, `Set-Cookie`
- Request control headers: `Host`, `Cache-Control`, `Trailer`

Sending forbidden headers as trailers is **undefined behavior** and may break clients or intermediaries.

**Best practices**:

- Use custom header names with `X-` prefix or domain-specific names
- Suitable trailer use cases: checksums, signatures, row counts, timestamps, processing metadata
- Keep trailer count and size modest (no hard limit, but consider client parsing overhead)

#### Trailer Testing

Comprehensive test coverage includes:

- Buffered response trailers: 7 tests validating constraints, multiple trailers, empty values, chaining
- Streaming response trailers: 5 tests validating chunked emission, fixed-length rejection, late addition
- Integration tests verifying wire format compliance with RFC 7230 §4.1.2

## Connection Close Semantics

| Mode | Meaning | Triggers |
|------|---------|----------|
| None | Connection reusable | Normal success |
| DrainThenClose | Flush pending then close | Client `Connection: close`, keep-alive limit, explicit handler intent |
| Immediate | Abort promptly | Parse/protocol error, size breach, transport failure |

Handlers normally rely on automatic policy; unrecoverable errors escalate to Immediate.

Keep-alive can be disabled globally by `cfg.withKeepAliveMode(false)`; per-request `Connection: close` or `Connection: keep-alive` headers are also honored (HTTP/1.1 default keep-alive, HTTP/1.0 requires explicit header).

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

### Graceful drain lifecycle

`SingleHttpServer` exposes a lifecycle state machine to coordinate shutdown:

| State | Description | Entered via |
|-------|-------------|-------------|
| Idle | Listener closed, loop inactive | Default / after drain/stop |
| Running | Event loop servicing connections | `run()` / `runUntil()` |
| Draining | Listener closed; existing connections finish with `Connection: close` | `beginDrain()` or signal-driven auto-drain |
| Stopping | Immediate teardown, pending connections closed | `stop()` or fatal epoll error |

Key API points:

- **`beginDrain(std::chrono::milliseconds maxWait = 0)`** stops accepting new connections, keeps existing keep-alive sessions long enough to finish their current response, and injects `Connection: close` so the client does not reuse the socket. When `maxWait` is non-zero, a deadline is armed; any connections still open when it expires are closed immediately. Calling `beginDrain()` again with a shorter timeout shrinks the deadline.
- **`isDraining()`** reflects whether the server is currently in the draining state. `isRunning()` still reports `true` until the drain completes or a stop occurs.
- **Wrappers** — `HttpServer::beginDrain()` / `isDraining()` forward to the underlying `SingleHttpServer` instances, enabling the same graceful drain flow when the server runs on background threads or across multiple reactors.
- Draining is restart-friendly: once all connections are gone (or the deadline forces closure) the lifecycle resets to `Idle` and the server can be started again with another `run()`.
- `stop()` remains the immediate shutdown primitive; it transitions to `Stopping`, force-closes all connections and wakes the event loop right away.

This drain lifecycle allows supervisors to quiesce traffic (e.g., removing an instance from load balancers) while letting outstanding requests complete and optionally bounding the wait for stubborn clients.

#### Signal-driven automatic drain

`aeronet` provides a global signal handler mechanism that coordinates graceful shutdown across all `SingleHttpServer` instances in the process:

```cpp
#include <aeronet/aeronet.hpp>

using namespace aeronet;

// Install process-wide signal handlers for SIGINT/SIGTERM
SignalHandler::Enable(std::chrono::milliseconds{5000});  // 5s max drain

SingleHttpServer server1(HttpServerConfig{});
SingleHttpServer server2(HttpServerConfig{});
// Both servers will automatically call beginDrain(5s) when SIGINT/SIGTERM is received
```

Key characteristics:

- **Process-wide coordination**: `SignalHandler::Enable()` installs `SIGINT` and `SIGTERM` handlers that set a global `sig_atomic_t` flag, visible to all `SingleHttpServer` instances.
- **Automatic drain**: Each server's event loop checks `SignalHandler::IsStopRequested()` at the end of every iteration (after lifecycle state checks) and calls `beginDrain(maxDrainPeriod)` if the flag is set.
- **Thread-safe**: The signal handler only sets an atomic flag; the actual drain logic runs from each server's event loop thread.
- **Multi-server friendly**: Unlike per-server signal handling (which has races where only the first reader consumes the signal), the global flag ensures all servers in the process see the stop request simultaneously.
- **Optional**: Applications that manage signals centrally can skip `SignalHandler::Enable()` and call `beginDrain()` directly as needed.

This mechanism is recommended for most deployments where a clean shutdown on `SIGINT`/`SIGTERM` is desired without writing custom signal-handling code. For containerized environments (Kubernetes, Docker), it ensures that `SIGTERM` (sent by orchestrators during pod shutdown) triggers a graceful drain with a bounded timeout, improving availability during rolling updates.

Where to look: [`signal-handler.hpp`](../aeronet/tech/include/aeronet/signal-handler.hpp), [`http-server-lifecycle_test.cpp`](../tests/http-server-lifecycle_test.cpp) for signal-driven drain tests.

#### stop() vs beginDrain() — intent, semantics and guidance

The library exposes two related shutdown controls and they serve different intent: `stop()` is the immediate termination primitive while `beginDrain()` explicitly requests a graceful quiesce. The differences are summarized below to avoid confusion.

- Semantics:
  - `stop()`:
    - Non‑blocking request to terminate the event loop as soon as practical.
    - Closes the listening socket and transitions the server into `Stopping` where connections are closed and the loop wakes to exit quickly.
    - Intended for cases where you want the server to stop servicing immediately (e.g. fatal error, process shutdown).
  - `beginDrain(maxWait)`:
    - Non‑blocking request to begin a graceful drain.
    - Closes the listening socket so no new connections are accepted, marks existing keep‑alive connections to be closed after their current response, and injects `Connection: close` so clients do not reuse the socket.
    - When `maxWait > 0` a deadline is armed; any remaining idle connections are forcibly closed when the deadline expires.

- Observability & lifecycle:
  - `isDraining()` becomes true after `beginDrain()` and remains true until the drain completes (or the deadline forces closure).
  - `isRunning()` remains true while the server's event loop is still executing; it becomes false after the loop returns (either naturally after drain completes or after `stop()`).

- Blocking vs non‑blocking:
  - Both `stop()` and `beginDrain()` are non‑blocking control requests in the current API. If consumers want synchronous semantics they must explicitly wait (e.g. monitor `isDraining()`/`isRunning()` or join the thread that runs the server).

- Typical usage patterns:
  - Graceful shutdown (recommended when you can wait or use a supervisor):
    1. Remove instance from load balancer.
    2. Call `beginDrain(maxWait)` to allow in‑flight requests to finish and bound the wait.
    3. Optionally wait for `isDraining()` -> `isRunning()` transition (or stop the wrapper thread) before exiting process.
  - Immediate teardown (fast exit / fatal conditions):
    - Call `stop()` to request immediate termination; the server will close connections promptly.

- Wrapper behavior:
  - `HttpServer::beginDrain()` forward to their underlying `SingleHttpServer` instances so the same graceful behavior is available for background or multi‑reactor setups. `stop()` continues to request immediate termination on wrappers as before.

Recommendation: prefer `beginDrain()` when you intend to quiesce traffic and let outstanding requests complete; use `stop()` when you require immediate termination. If you need a blocking API (wait until drain completes), add a small wait in the supervisor code that observes `isDraining()`/`isRunning()` or joins the server thread — the public API intentionally separates "request" (non‑blocking) from "wait" to keep shutdown control explicit.

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

You can define global headers applied to every response of a `SingleHttpServer` via `HttpServerConfig.globalHeaders`. These are appended after any user-set headers in a handler, so you can override them per-response if needed. Useful for consistent security headers (CSP, HSTS, etc). They will not override any header of the same name already set in a response.

Global headers are applied to every response including error responses generated internally by aeronet (400, 413, etc).

By default, it contains a `Server: aeronet` header unless you explicitly clear it out.

## Path Handling

### Query String & Parameters

- Path percent-decoded once; invalid escape ⇒ 400.
- Query left raw; per-key/value decode on iteration (`queryParams()`).
- `+` converted to space only in query pairs.
- Missing `=` ⇒ empty value; duplicates preserved.
- Malformed escapes in query components surfaced literally (non-fatal).

Example:

```cpp
Router router;
router.setPath(http::Method::GET, "/users/{id}", [](const HttpRequest& req) {
  for (auto [k, v] : req.queryParams()) { /* use k,v */ }
  return HttpResponse(200);
});
```

## Middleware Pipeline

- **Global hooks** – use `Router::addRequestMiddleware` and `Router::addResponseMiddleware` (or the convenience `SingleHttpServer::add*` wrappers) to install request/response middleware that runs for every request.
- **Per-route hooks** – `PathHandlerEntry::before(RequestMiddleware)` and `::after(ResponseMiddleware)` scope middleware to a specific path registration.
- **Execution order** – `global pre → route pre → handler → route post → global post`. When a route does not match (404/405/redirect), only the global hooks run; per-route chains are skipped.
- **Short-circuiting** – returning `MiddlewareResult::ShortCircuit(HttpResponse)` from any pre middleware skips the remaining pre chain and the handler. The produced response is still passed through the post chain so that shared concerns (headers, logging, metrics) execute uniformly.
- **Threading** – middleware executes on the server's event loop thread; avoid blocking work inside hooks.

### Streaming Integration

- `HttpResponseWriter` driven handlers share the same middleware semantics. Post middleware runs right before headers are flushed, allowing status and header mutation even when body chunks were emitted.
- Automatic CORS headers are applied after middleware adjustments, mirroring buffered responses.
- Synthetic responses generated before the handler (CORS denials, 406 content-coding fallback, pre-chain short-circuits) still traverse the post middleware chain.

### Coroutine Handlers (Async)

**aeronet** supports C++20 coroutines for request handling, allowing you to write asynchronous code that looks synchronous. This is particularly useful when your handler needs to perform asynchronous operations (like database queries, upstream HTTP requests, or timers) without blocking the event loop thread.

#### Key Concepts

- **Signature**: Handlers return `RequestTask<HttpResponse>` instead of `HttpResponse`.
- **Registration**: Use `Router::setPath` just like normal handlers. The router automatically detects the return type.
- **Execution**: The coroutine is started immediately on the event loop. When it `co_await`s, it suspends, returning control to the event loop. When the awaited operation completes, the coroutine resumes.
- **Middleware**: Fully supported. Request middleware runs before the coroutine starts. Response middleware runs after the coroutine `co_return`s the response.
- **CORS**: Fully supported. CORS checks happen before the coroutine starts.
- **Early Dispatch**: Async handlers are invoked as soon as the request head is parsed, even if the body is still uploading. Call `co_await req.bodyAwaitable()` (or the chunk helpers) before touching the body. Because of this, request middleware on async routes should not rely on the body or trailers being populated—they will become available only after the coroutine awaits them.

#### Async Handler Example

```cpp
using namespace aeronet;

struct User { int id; /* ... */ };

// A hypothetical async database client
// Minimal awaitable used for the demo: provides the three awaiter
// methods so it can be consumed with `co_await` inside an async handler.
struct GetUserAwaitable {
  int id;
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> handle) noexcept { handle.resume(); }
  User await_resume() const noexcept { return User{id}; }
};

GetUserAwaitable getUserAsync(int id) {
  return GetUserAwaitable{id};
}

int main() {
  Router router;

  // Register an async handler
  router.setPath(http::Method::GET, "/users/{id}", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    // 1. Parse parameters (synchronous)
    int userId = std::stoi(std::string(req.pathParams().at("id")));

    // 2. Suspend while fetching data (non-blocking)
    // The event loop is free to handle other requests while we wait.
    User user = co_await getUserAsync(userId);

    // 3. Resume and build response
    co_return HttpResponse(200).body(std::to_string(userId));
  });

  // Async body reading
  router.setPath(http::Method::POST, "/upload", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    // Wait for the full body to be received
    std::string_view body = co_await req.bodyAwaitable();
    
    co_return HttpResponse(200).body("Received " + std::to_string(body.size()) + " bytes");
  });

  SingleHttpServer server(HttpServerConfig{}, std::move(router));
  server.run();
}
```

When a route uses an async handler, request middleware may observe an empty body/trailer map because aggregation now happens in parallel with handler execution; apply validation inside the coroutine if the middleware needs the payload.

#### Awaitables

You can `co_await` any type that satisfies the C++ coroutine awaitable concept.
**aeronet** provides built-in awaitables:

- `req.bodyAwaitable()`: Suspends until the full request body is available (buffered).
- `req.readBodyAsync(maxBytes)`: (Future) Suspends until a chunk of body data is available.

#### Implementation Details

- **Return Type**: `RequestTask<T>` is a lightweight task object. For handlers, `T` must be `HttpResponse`.
- **Exception Handling**: Exceptions thrown within the coroutine (before the first suspension or after resumption) are caught by the server infrastructure and result in a 500 Internal Server Error, just like synchronous handlers.
- **Thread Safety**: The coroutine resumes on the same thread (the event loop). You don't need locks to access server state, but you must ensure your async operations (like the DB client in the example) are thread-safe or properly synchronized if they use other threads.

### Middleware Example

```cpp
auto isAuthenticated = [](const HttpRequest &req) { return true; };  // user-defined

Router router;
router.addRequestMiddleware([isAuthenticated](HttpRequest& req) {
  if (!isAuthenticated(req)) {  // user-defined helper
    HttpResponse resp(http::StatusCodeUnauthorized);
    resp.body("auth required");
    return MiddlewareResult::ShortCircuit(std::move(resp));
  }
  return MiddlewareResult::Continue();
});

router.addResponseMiddleware([](const HttpRequest&, HttpResponse& resp) {
  resp.header("X-Powered-By", "aeronet");
});

auto renderMetrics = []() { return std::string{}; };  // user-defined

auto& entry = router.setPath(http::Method::GET, "/metrics", [renderMetrics](const HttpRequest&) {
  HttpResponse resp;
  resp.body(renderMetrics());  // user-defined helper
  return resp;
});

entry.before([](HttpRequest& req) {
  // tagRequest(req, "metrics");  // user-defined helper
  return MiddlewareResult::Continue();
});

entry.after([](const HttpRequest&, HttpResponse& resp) {
  resp.header("Cache-Control", "no-store");
});

SingleHttpServer server(HttpServerConfig{}, std::move(router));
```

### Middleware Metrics Callback

- `SingleHttpServer::setMiddlewareMetricsCallback(MiddlewareMetricsCallback)` installs an opt-in hook that receives a
  `MiddlewareMetrics` record for every middleware invocation. The record captures whether the middleware belongs to the
  global or per-route chain, the execution phase (`Pre` or `Post`), the zero-based index within that chain, whether the
  middleware short-circuited request processing, threw an exception, and how long the call lasted in nanoseconds.
- Metrics are emitted for both buffered and streaming handlers; the `streaming` flag is set when the active route uses
  `HttpResponseWriter`. Request method and the canonical request path are included to simplify downstream tagging.
- When no callback is registered, the server skips the timing code paths entirely to keep the hot path allocation-free
  and avoid the additional steady clock reads.
- Tests: see `tests/http-routing_test.cpp` (`HttpMiddlewareMetrics.RecordsPreAndPostMetrics`,
  `HttpMiddlewareMetrics.MarksShortCircuit`, `HttpMiddlewareMetrics.StreamingFlagPropagates`).

### Related Tests

- See `tests/http-routing_test.cpp` for examples covering ordering, short-circuits, and streaming responses.

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

Tests: `tests/http_routing_test.cpp`.

Usage:

```cpp
RouterConfig routerConfig;
routerConfig.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect);
```

Rationale: Normalize avoids duplicate handler registration while preserving SEO-friendly consistent canonical paths; Redirect enforces consistent public URLs; Strict maximizes explicitness (APIs where `/v1/resource` vs `/v1/resource/` semantics differ).

### Routing patterns & path parameters

- Path pattern syntax

  - Paths are absolute and must begin with `/`.
  - A path is split into segments by the `/` character. Each segment may be:
    - A literal segment with no braces (e.g. `hello`, `v1`).
    - A pattern segment containing parameter fragments interleaved with literals. Example: `v{}/foo{}bar`.
    - A terminal wildcard segment `*` which must be the final segment in the pattern (e.g. `/files/*`).

- Parameter fragments

  - Named captures use `{name}` and become available under the provided key (`name`).
  - Unnamed captures use `{}`; the router assigns sequential numeric string keys (`"0"`, `"1"`, ...) in segment order.
  - Mixing named and unnamed captures in the same pattern is not allowed — registration (`setPath`) will throw if you mix them.
  - Consecutive parameter fragments with no literal separator (e.g. `{}{}` within a segment) are rejected.
  - If you want to have literal braces in a segment, escape them by doubling: `{{` and `}}` become `{` and `}` respectively.

- Wildcard semantics

  - `*` must appear alone in a segment and must be the final segment. A wildcard matches the remainder of the
    path but does not populate path-parameter captures.
  - Exact registrations take precedence over wildcard matches (e.g. `/a/b` wins over `/a/*` for `/a/b`).

- Registration errors

  - `setPath()` will throw on:
    - pattern not starting with `/`
    - empty segment (double slash `//`)
    - unterminated `{` in a segment
    - consecutive parameters without a literal separator
    - wildcard `*` used in a non-terminal position
    - mixing named and unnamed parameters within the same pattern
    - conflicting parameter naming or wildcard usage for an identical registered pattern

- Matching & capture lifetime

  - Patterns are compiled at registration; matching returns captures as `string_view`s (no copies of the captured
    substrings). Captures returned by the router are transient and reference the original request path buffer
    and the router's internal transient storage.
  - Callers must copy captured values if they need them to survive beyond the original request buffer lifetime or
    beyond a subsequent `match()` call which may mutate the router's transient buffers.

- How to retrieve path params from handlers

  - When `SingleHttpServer` dispatches to a handler, it copies routing captures into the `HttpRequest` object. Within
    your handler you can access them via `req.pathParams()` which returns a `flat_hash_map<std::string_view, std::string_view>`.
  - Example:

```cpp
Router router;
router.setPath(http::Method::GET, "/users/{id}/posts/{post}", [](const HttpRequest& req) {
  auto params = req.pathParams();
  auto it = params.find("id");
  if (it != params.end()) {
    std::string_view userId = it->second; // points into request buffer
    // copy if you need to keep it beyond request lifetime: std::string(userId)
  }
  return HttpResponse(200, "OK");
});
```

- Unnamed capture example (keys are "0", "1", ...):

```cpp
Router router;
router.setPath(http::Method::GET, "/files/{}/chunk/{}", [](const HttpRequest&) {
  return HttpResponse(200, "OK");
});
// In handler: req.pathParams().at("0"), req.pathParams().at("1")
```

## Construction Model (RAII & Ephemeral Ports)

`SingleHttpServer` binds, configures the listening socket and registers it with epoll inside its constructor (RAII). If you request an ephemeral port (`port = 0`), the kernel-assigned port is immediately available via `server.port()` after construction (no separate setup step).

Why RAII:

- Fully initialized, listening server object or an exception (no half states)
- Simplifies lifecycle and tests (ephemeral port resolved synchronously)
- Enables immediate inspection / registration before running

Ephemeral pattern:

```cpp
HttpServerConfig cfg; // port left 0 => ephemeral
SingleHttpServer server(cfg);
uint16_t actual = server.port();
```

Restart semantics: Both `SingleHttpServer` and `HttpServer` support restart via `run()` after a prior `stop()` or completed `beginDrain()`. The listening socket and reactor state are rebuilt on each `run()` call, allowing reuse of the same server object across multiple start/stop cycles.

Removed experimental factory: a previous non-throwing `tryCreate` was dropped to keep API surface minimal.

Design trade-offs: Constructor may throw on errors (bind failure, TLS init failure if configured). This is intentional to surface unrecoverable configuration issues early.

### Copy semantics (SingleHttpServer & MultiHttpServer)

- `SingleHttpServer` supports copy construction and copy assignment while the source instance is fully stopped. Copy assignment automatically calls `stop()` on the destination before duplicating sockets and router state; attempting to copy from a running instance throws `std::logic_error` to avoid duplicating active event loops. When copying bound sockets, ensure the original server either relinquishes the port (call `stop()` or destroy the instance) or has `reusePort=true` so the new copy can bind successfully.
- `MultiHttpServer` mirrors the same rule: copies are only allowed from a stopped source. Copy construction and assignment rebuild fresh `SingleHttpServer` instances carrying the same port, router, and thread count while wiring them to a new lifecycle tracker. Running sources throw `std::logic_error` to prevent duplicating active thread pools.
- Tests: see `tests/http-server-lifecycle_test.cpp` (`HttpServerCopy.*`) and `tests/multi-http-server_test.cpp` (`MultiHttpServerCopy.*`).

## HttpServer lifecycle

Manages N reactors via `SO_REUSEPORT`.

### In a nutshell

- Constructor binds & resolves port (ephemeral resolved once).
- Restart rebuilds underlying single‑shot servers; same port reused.
- Modify handlers only while stopped (between stop/start).
- `reusePort=true` required for `threadCount > 1`.
- Movable even while running (vector storage stable).
- Graceful drain propagates: `beginDrain(maxWait)` stops all accept loops, existing keep-alive connections receive `Connection: close`, and `isDraining()` reports when any underlying instance is still draining.

### HttpServer restart example

```cpp
Router router;
router.setDefault([](const HttpRequest&){ return HttpResponse(200,"OK").body("hi\n"); });
HttpServerConfig cfg;
cfg.nbThreads = 4;
HttpServer multi(cfg, std::move(router));
// Use `start()` as a void convenience which manages an internal handle. Use `startDetached()` if you need
// an `AsyncHandle` to control or inspect the background threads explicitly.
multi.start();
multi.stop();
multi.start();
```

### Port reuse semantics

The library interprets this boolean slightly differently depending on whether you use a single `SingleHttpServer` or the `MultiHttpServer` wrapper to make the behaviour both safe and intuitive:

- Single `SingleHttpServer`:
  - `reusePort = false` creates the listening socket without that reuse option.
  - `reusePort = true` requests the kernel-level reuse option (platform dependent: SO_REUSEPORT/SO_REUSEADDR) when creating the listening socket for that server instance.

- `MultiHttpServer` (multi-reactor wrapper):
  - `reusePort = false` (recommended for explicit ports): the first server binds the explicit port exclusively (no reuse option) temporarily to ensure the process obtains the port and avoid accidentally binding to an unrelated process. Once the exclusive bind succeeds, subsequent internal sibling servers created by `MultiHttpServer` will be started to reuse that resolved port internally for multi-reactor operation. This gives a safe default for explicit ports while still providing multi-reactor scaling inside the process.
  - `reusePort = true`: no check about possible existing listener on the system on the given port is made. The first server will set the reuse option and all servers created for the `MultiHttpServer` will use socket reuse. This enables binding by other co-located processes as well as in-process siblings.

Note: ephemeral ports (`port == 0`) preserve prior behaviour: the first server discovers the kernel-assigned ephemeral port and subsequent siblings bind to that resolved port using reuse semantics so `MultiHttpServer` keeps working with ephemeral port allocation.

To sum-up, for most cases you will prefer `reusePort = false` (which is the default) to avoid accidental port conflicts with other processes and keep your own server instances listening for trafic, while still getting multi-reactor scaling internally. Use `reusePort = true` only when you explicitly want to share the port with other processes or have specific reuse semantics in mind.

## Built-in Kubernetes-style probes

`aeronet` can optionally provide a small set of built-in HTTP probe endpoints intended to be used
by Kubernetes-style health checks and load-balancers. These probes are lightweight, handled
entirely by the server, and do not require application handlers to be installed when enabled.

### Probes in a nutshell

- Enabled via `HttpServerConfig::withBuiltinProbes(BuiltinProbesConfig)` or `enableBuiltinProbes(true)`.
- Default probe paths (configurable in `BuiltinProbesConfig`):
  - Liveness: `/livez` — consistently returns HTTP 200 while the server is running
  - Readiness: `/readyz` — indicates the server is ready to receive new requests (HTTP 200). During draining, it returns HTTP 503 and returns `Connection: close` to signal clients not to reuse connections.
  - Startup: `/startupz` — returns HTTP 503 until the server has fully initialized, then returns HTTP 200 like liveness.
- The probe handlers return minimal responses (status only, configurable Content-Type) and avoid heavy work.

### Probes configuration options

- `BuiltinProbesConfig::enabled` (bool): enable/disable builtin probes.
- `BuiltinProbesConfig::contentType` (enum): response Content-Type used by the probe responses.
- `BuiltinProbesConfig::withLivenessPath / withReadinessPath / withStartupPath`: customize probe paths. Paths must be
  non-empty and begin with `/` — invalid values are rejected by `BuiltinProbesConfig::validate()`.

When enabled, if an application handler is already registered on the same path(s) the server will override them
with the probes handlers.

### Probes Notes & recommendations

- Builtin probes are intentionally tiny and designed for readiness/liveness checks only. If you need richer
  health diagnostics (dependencies, DB, caches), implement a custom application handler and register it on a
  non-conflicting path.
- Enabling builtin probes is useful for quick deployments and reduces application boilerplate. If you prefer
  full control or want to return structured JSON diagnostics, disable builtin probes and register your own
  handlers.

### Probes configuration example

Enable builtin probes with default paths and a plain-text content type:

```cpp
HttpServerConfig cfg;
BuiltinProbesConfig probesCfg;
probesCfg.enabled = true;
probesCfg.contentType = BuiltinProbesConfig::ContentType::TextPlainUtf8;
probesCfg.withLivenessPath("/livenessz");
probesCfg.withReadinessPath("/readinessz");
probesCfg.withStartupPath("/startupz");

cfg.withBuiltinProbes(std::move(probesCfg));
SingleHttpServer server(std::move(cfg));
```

### Testing

- The test suite includes `http_probes_test.cpp` which validates startup/readiness transitions and drain-time
  behavior. Tests also cover collision detection for probe paths.

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
| Kernel TLS (kTLS) sendfile | ✅ | Linux-only zero-copy sendfile for TLS sockets; enabled by default with graceful fallback. |
| Handshake timeout | ✅ | `withTlsHandshakeTimeout(ms)` closes stalled handshakes |
| Graceful TLS shutdown | ✅ | Best‑effort `SSL_shutdown` before close |
| ALPN strict mismatch counter | ✅ | Per‑server stats |
| Handshake success counter | ✅ | Per‑server stats |
| Client cert presence counter | ✅ | Per‑server stats |
| ALPN distribution | ✅ | Vector (protocol,count) in stats |
| TLS version distribution | ✅ | Stats field |
| Cipher distribution | ✅ | Stats field |
| Handshake duration metrics | ✅ | Count / total ns / max ns |
| Handshake failure reason buckets | ✅ | `ServerStats::tlsHandshakeFailureReasons` |
| Handshake event callback | ✅ | `SingleHttpServer::setTlsHandshakeCallback()` (and `MultiHttpServer::setTlsHandshakeCallback()`) |
| JSON stats export | ✅ | `serverStatsToJson()` includes TLS metrics |
| No process‑global mutable TLS state | ✅ | All metrics per server instance |
| Session resumption (tickets) | ✅ | Server-side TLS session ticket support with automatic key rotation. |
| Handshake full/resumed counters | ✅ | `ServerStats::{tlsHandshakesFull,tlsHandshakesResumed}` |
| Handshake admission control | ✅ | Concurrency limit + basic rate limiting via `TLSConfig::{maxConcurrentHandshakes,handshakeRateLimitPerSecond,handshakeRateLimitBurst}` |
| MultiHttpServer shared ticket keys | ✅ | Session ticket key store is shared across instances for consistent resumption |
| SNI multi-cert routing | ✅ | `TLSConfig::withTlsSniCertificate*()` selects cert by SNI |
| Hot cert/key reload (atomic swap) | ✅ | `postConfigUpdate()` with TLS config change rebuilds and swaps TLS context for new connections |
| Dynamic trust store update | ✅ | `postConfigUpdate()` with trust store change |
| OCSP stapling / revocation checks | ⏳ | OCSP staple responses & revocation checking with caching. |

### TLS Configuration Example

```cpp
HttpServerConfig cfg;
std::string certPem /* = R"(-----BEGIN CERTIFICATE----- */;
std::string keyPem  /* = R"(-----BEGIN PRIVATE KEY-----*/;
cfg.withPort(8443)
   .withTlsCertKeyMemory(certPem, keyPem)
   .withTlsAlpnProtocols({"http/1.1"})
   .withTlsAlpnMustMatch(true)
   .withTlsMinVersion("TLS1.2")
   .withTlsMaxVersion("TLS1.3")
   .withTlsHandshakeTimeout(std::chrono::milliseconds(750));
SingleHttpServer server(cfg);
```

Strict ALPN: if enabled and no protocol overlap, handshake aborts (connection closed, metric incremented).

### Handshake event callback

You can subscribe to handshake outcomes (succeeded / failed / rejected) with a lightweight callback.

```cpp
SingleHttpServer server;
server.setTlsHandshakeCallback([](const TlsHandshakeEvent& ev) {
  // ev.result is one of: Succeeded, Failed, Rejected
  // ev.reason is a short string for failures/rejections (best-effort)
  // ev.selectedAlpn / negotiatedCipher / negotiatedVersion are filled for successful handshakes
  if (ev.result == TlsHandshakeEvent::Result::Failed) {
    log::warn("TLS handshake failed fd={} reason={} ver={} cipher={}", ev.fd, ev.reason,
              ev.negotiatedVersion, ev.negotiatedCipher);
  }
});
```

### Handshake failure reason buckets (stats)

`ServerStats::tlsHandshakeFailureReasons` provides best-effort bucketing of why handshakes failed or were rejected.

```cpp
SingleHttpServer server;
const ServerStats st = server.stats();
for (const auto& kv : st.tlsHandshakeFailureReasons) {
  const std::string& reason = kv.first;
  const uint64_t count = kv.second;
  log::info("tls_handshake_reason={} count={}", reason, count);
}
```

### Hot reload (cert/key) and dynamic trust store update

New connections pick up the updated TLS configuration after the next poll cycle.
Use `postConfigUpdate()` to modify TLS settings at runtime - changes are detected
and the SSL context is atomically rebuilt for new connections.

```cpp
SingleHttpServer server;

// Hot swap certificate and key
TLSConfig tls;
tls.enabled = true;
std::string_view newCertPem /* = R"(-----BEGIN CERTIFICATE-----"*/;
std::string_view newKeyPem  /* = R"(-----BEGIN PRIVATE KEY-----"*/; 
tls.withCertPem(newCertPem).withKeyPem(newKeyPem);
server.postConfigUpdate([tls = std::move(tls)](HttpServerConfig& cfg) mutable {
  cfg.tls = std::move(tls);
});

// Or just update the trust store (clears existing, adds new)
std::string_view newClientCaPem /* = R"(-----BEGIN CERTIFICATE-----"*/;
server.postConfigUpdate([newClientCaPem](HttpServerConfig& cfg) {
  cfg.tls.withoutTlsTrustedClientCert().withTlsTrustedClientCert(newClientCaPem);
});
```

### Kernel TLS (kTLS) sendfile

**aeronet** supports kTLS when supported by the system. It will attempt to enable kernel TLS sendfile on
each TLS connection. The default mode is **Opportunistic**: the server opportunistically enables kTLS and will fall back silently to
the existing user-space TLS path if the kernel, OpenSSL, or negotiated cipher suite does not support it.
It increments the `ktlsSendEnableFallbacks` counter when offload is unavailable so operators are informed about the reason for fallback.
Use `Required` to treat offload failure as fatal.
Configuration lives in `TLSConfig::ktlsMode`, exposed via `HttpServerConfig::withTlsKtlsMode(...)` with the
following modes:

| Mode | Behaviour |
|------|-----------|
| `Disabled` | Never attempt kTLS. |
| `Opportunistic` (default) | Attempt once per connection; on failure fall back to user-space TLS. |
| `Enabled` | Same as `Opportunistic`, but emits a warning log in case of failure to set kTLS during the handshake. |
| `Required` | Treat failure/unsupported as fatal and close the connection immediately. |

Runtime counters (`ServerStats`) report how many connections enabled kTLS, how many fell back, forced shutdowns, and
the aggregate bytes transferred via kernel TLS sendfile. Logs capture the reason for any fallback.

#### How to enable kTLS in your server

1. Ensure the project was compiled with both `AERONET_ENABLE_OPENSSL=ON` and uses a modern OpenSSL version (>= 3.0).
1. Provide TLS credentials — either files or in-memory PEM strings — exactly as for any HTTPS deployment.
1. Set the desired kTLS mode before constructing the server:

```cpp
HttpServerConfig cfg;
cfg.withPort(8443)
   .withTlsCertKey("/path/to/fullchain.pem", "/path/to/privkey.pem")
   .withTlsKtlsMode(TLSConfig::KtlsMode::Opportunistic);  // or Disabled / Enabled / Required
SingleHttpServer server(cfg);
```

1. Optionally consult `server.stats()` to monitor `ktlsSendEnabledConnections`, `ktlsSendEnableFallbacks`,
  and `ktlsSendBytes` during runtime. These counters help verify whether your kernel accepted offload or if the
  code fell back to the classic user-space TLS path.

See `examples/tls-ktls.cpp` for a runnable end-to-end snippet combining all of the above.

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
| `withTlsTrustedClientCert(pem)` | Append trust anchor (repeatable) |

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

Roadmap (see also table above): SNI routing, hot reload of cert/key, OCSP / revocation checks.

### TLS Session Tickets

Session tickets allow TLS session resumption without server-side session caches, enabling faster subsequent handshakes (0-RTT negotiation). aeronet provides automatic key management with configurable rotation.

#### Session Ticket Concepts

- **Session Tickets**: Encrypted session state sent to the client, allowing resumption without a full handshake.
- **Ticket Encryption Keys**: 48-byte keys (16B key name + 16B AES key + 16B HMAC key) used to encrypt/decrypt tickets.
- **Key Rotation**: Automatic rotation prevents stale keys from being used indefinitely.

#### Configuration Options

| Method | Purpose |
|--------|---------|
| `withTlsSessionTickets(true)` | Enable session tickets (default: disabled) |
| `withTlsSessionTicketLifetime(duration)` | Key rotation interval (default: 1 hour) |
| `withTlsSessionTicketMaxKeys(n)` | Maximum keys in rotation (default: 3) |
| `withTlsSessionTicketKey(key)` | Load a static 48-byte key (disables rotation) |

#### Automatic Key Rotation

When enabled without a static key, `aeronet` generates cryptographically random keys and rotates them automatically:

```cpp
HttpServerConfig cfg;
cfg.withPort(8443)
   .withTlsCertKey("cert.pem", "key.pem");
cfg.tls.withTlsSessionTickets(true)
       .withTlsSessionTicketLifetime(std::chrono::hours{2})
       .withTlsSessionTicketMaxKeys(4);

SingleHttpServer server(std::move(cfg));
```

This configuration:

- Generates new keys every 2 hours
- Keeps up to 4 keys for decrypting older tickets during rotation
- Automatically purges keys beyond the maximum

#### Static Key Loading

For deployments requiring key consistency across restarts or multiple server instances, load a static key:

```cpp
// 48-byte key: 16B name + 16B AES + 16B HMAC
TLSConfig::SessionTicketKey keyData{};

// populate keyData securely (e.g., from a secrets manager)

HttpServerConfig cfg;
cfg.withPort(8443)
   .withTlsCertKey("cert.pem", "key.pem");
cfg.tls.withTlsSessionTicketKey(keyData);  // Enables tickets + loads key

SingleHttpServer server(std::move(cfg));
```

When a static key is provided:

- Session tickets are automatically enabled
- Key rotation is disabled (only the static key is used)
- The same key can be shared across server instances for distributed session resumption

#### Security Considerations

- **Key Size**: Each key is exactly 48 bytes (`TLSConfig::kSessionTicketKeySize`).
- **Key Generation**: Uses `RAND_bytes()` for cryptographically secure random generation.
- **Key Storage**: Static keys should be stored securely (e.g., secrets manager, encrypted storage).
- **Rotation**: Regular rotation limits the impact of key compromise; shorter lifetimes = better security.
- **OpenSSL 3.0+**: Uses modern EVP_MAC API for HMAC operations.

#### Testing Session Tickets

Verify session resumption with OpenSSL's `s_client`:

```bash
# First connection (full handshake)
openssl s_client -connect localhost:8443 -sess_out session.pem

# Second connection (resumed)
openssl s_client -connect localhost:8443 -sess_in session.pem
# Look for "Reused, TLSv1.3" in output
```

See `examples/tls-session-tickets.cpp` for a complete working example.

---

### TRACE method policy

The server exposes a configurable `TracePolicy` to control handling of the HTTP `TRACE` method. Use
`HttpServerConfig::withTracePolicy(...)` to choose one of:

- `Disabled` (default) — reject TRACE (405).
- `Disabled` (default) — reject TRACE (405).
- `EnabledPlainAndTLS` — allow TRACE and echo the received request message (RFC 7231 §4.3) on both plaintext and TLS.
- `EnabledPlainOnly` — allow TRACE on plaintext connections only; reject when the request arrived over TLS.

This provides a safety-minded default while allowing deployments to express site-specific policies (e.g. disallow TRACE on TLS).

Quick reference matrix:

| Policy | Plaintext TRACE | TLS TRACE | Description |
|--------|-----------------|-----------|-------------|
| Disabled | Rejected (405) | Rejected (405) | Default safe option — TRACE not allowed |
| Enabled  | Allowed (echo)  | Allowed (echo) | TRACE permitted on all transports |
| EnabledPlainOnly | Allowed (echo)  | Rejected (405) | Useful when TLS endpoints must not expose request echoes |
| EnabledPlainAndTLS  | Allowed (echo)  | Allowed (echo)  | TRACE allowed on both plaintext and TLS |

Examples:

- To disable TRACE entirely (default): `cfg.withTracePolicy(HttpServerConfig::TracePolicy::Disabled);`
To allow TRACE only on plaintext: `cfg.withTracePolicy(HttpServerConfig::TracePolicy::EnabledPlainOnly);`
To allow TRACE on both plaintext and TLS: `cfg.withTracePolicy(HttpServerConfig::TracePolicy::EnabledPlainAndTLS);`

## Streaming Responses (Chunked / Incremental)

Handlers can produce bodies incrementally using a streaming handler registration instead of fixed responses. When streaming, headers are deferred until either a compression decision (if enabled) or first write.

Key semantics:

- Default transfer uses `Transfer-Encoding: chunked` unless `contentLength()` was called before any body writes.
- `write()` queues data; returns `false` only when the connection is marked to close (e.g. outbound buffer limit exceeded or fatal error). Future versions may introduce a "should-pause" state.
- `end()` finalizes, emitting terminating `0\r\n\r\n` in chunked mode and flushing any compression trailers.
- HEAD requests suppress body bytes automatically (still compute/send Content-Length when known).
- Keep-alive preserved if policy allows and no fatal condition occurred.
- Zero-copy file responses: both `HttpResponse::file(...)` and `HttpResponseWriter::file(...)` accept an
  `aeronet::File` descriptor and stream its contents with Linux `sendfile(2)` on plaintext sockets. When TLS is
  active, aeronet reuses the connection's tunnel buffer and feeds encrypted writes via `pread` + `SSL_write`, so no
  additional heap allocations are introduced beyond that shared buffer.
- `file` automatically wires `Content-Length`, rejects trailers/body mutations, and honors HEAD semantics (headers
  only, body suppressed).

Backpressure & buffering:

- Unified outbound queue for both fixed & streaming; immediate write path used when queue empty, else bytes accumulate and EPOLLOUT drives flushing.
- Exceeding `maxOutboundBufferBytes` marks connection to close after pending data flush (subsequent `write()` yields false).

Limitations (current phase): no trailer support; compression integration limited to buffered activation decision; inbound streaming decompression not yet implemented.

Example:

```cpp
Router router;
router.setDefault([](const HttpRequest&, HttpResponseWriter& w){
  w.status(200, "OK");
  w.header("Content-Type", "text/plain");
  for (int i=0;i<5;++i) {
    if (!w.writeBody("chunk-" + std::to_string(i) + "\n")) break;
  }
  w.end();
});

SingleHttpServer server(HttpServerConfig{}.withPort(8080), std::move(router));
```

Testing: see `tests/http_streaming.cpp`.

- [x] `StaticFileHandler` serves directory trees with zero-copy `file`
- [x] RFC 7233 single-range parsing and validation (`Range`, `If-Range`)
- [x] RFC 7232 validators (`If-None-Match`, `If-Match`, `If-Modified-Since`, `If-Unmodified-Since`)
- [x] Strong ETag generation (`size-lastWriteTime`), `Last-Modified`, `Accept-Ranges: bytes`
- [x] 416 (Range Not Satisfiable) with `Content-Range: bytes */N`
- [x] Integration hooks in `HttpServerConfig::staticFiles`

## Static File Handler (RFC 7233 / RFC 7232)

`StaticFileHandler` provides a hardened helper for serving filesystem trees while respecting HTTP caching and range semantics.
The handler is designed to plug into the existing routing API: it is an invocable object that accepts an `HttpRequest` and returns an `HttpResponse`, so it works with `SingleHttpServer` and `MultiHttpServer` exactly like any other handler.

- **Zero-copy transfers**: regular GET requests use `HttpResponse::file()` so plaintext sockets reuse the kernel
  `sendfile(2)` path. TLS endpoints automatically fall back to the buffered write path that aeronet already uses for
  file responses.
- **Directory listings**: when `StaticFileConfig::enableDirectoryIndex` is true and no default index file is present,
  aeronet emits an HTML index with optional trailing-slash redirect, hidden-file filtering (`showHiddenFiles`),
  configurable CSS (`withDirectoryListingCss`) and a pluggable renderer (`directoryIndexRenderer`). Large directories
  obey `maxEntriesToList` and advertise truncation via `x-directory-listing-truncated: 1`.
- **Single-range support**: `Range: bytes=N-M` (RFC 7233 §2.1) is parsed with strict validation. Valid ranges return
  `206 Partial Content` with `Content-Range`. Invalid syntax returns `416` with `Content-Range: bytes */<size>` per the
  spec. Multi-range requests (comma-separated) are rejected as invalid.
- **Conditional requests**: `If-None-Match`, `If-Match`, `If-Modified-Since`, `If-Unmodified-Since`, and `If-Range`
  are honoured using strong validators. Requests that do not modify the resource return `304 Not Modified` for GET/HEAD
  or `412 Precondition Failed` for unsafe methods. `If-Range` transparently falls back to the full body when the
  validator mismatches.
- **Headers**: the handler always emits `Accept-Ranges: bytes` so clients learn range capability. `ETag` and
  `Last-Modified` are enabled by default (configurable) and share the same strong validator used by conditionals.

- **Content-Type resolution**: when serving files the handler resolves the `Content-Type` header with the following
  precedence: (1) a user-provided content-type resolver callback (if installed) and returning a non-empty value,
  (2) the configured default content type in `HttpServerConfig` (if non-empty), and (3) the hard fallback
  `application/octet-stream`. The library exposes `File::detectedContentType()` which performs filename-extension based
  detection using the bundled extension → mime table (the table was extended to include common C/C++ extensions such as
  `c`, `h`, `cpp`, `hpp`, `cc`). Applications with different heuristics (case-insensitive lookup, longest-suffix
  matching like `tar.gz`, etc.) can supply their own resolver to override the default behavior.
- **Safety**: all request paths are normalised under the configured root; `..` segments are rejected. Default index
  fallback (e.g. `index.html`) is configurable or can be disabled.
- **Config entry point**: the immutable configuration lives in `StaticFileConfig`. The handler constructor also accepts a config directly.

Example usage:

```cpp
#include <aeronet/aeronet.hpp>

using namespace aeronet;

int main() {
  HttpServerConfig cfg;
  cfg.withPort(8080);

  StaticFileConfig staticFileConfig;
  staticFileConfig.enableRange = true;
  staticFileConfig.addEtag = true;
  staticFileConfig.enableDirectoryIndex = true;  // fallback to HTML listings when index.html is absent
  staticFileConfig.withDefaultIndex("index.html");

  Router router;
  StaticFileHandler assets("/var/www/html", std::move(staticFileConfig));
  router.setPath(http::Method::GET, "/", [assets](const HttpRequest& req) mutable {
    return assets(req);
  });

  SingleHttpServer server(std::move(cfg), std::move(router));

  server.run();
}
```

Try it (build & run the example)

```bash
# from repository root
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target aeronet-static-example

# Run, optionally passing a port and a root directory to serve
./build/examples/aeronet-static-example 8080 ./examples/static-assets

# Test with curl (full file)
curl -i http://localhost:8080/somefile.txt

# Test single-range request
curl -i -H "Range: bytes=0-3" http://localhost:8080/somefile.txt
```

Testing lives in `tests/http_range_test.cpp` which exercises full-body responses, single-range `206`, unsatisfiable
requests, `If-None-Match`, and `If-Range`. Those tests rely on the same public API shown above, ensuring the feature is
covered end-to-end.

- [ ] Multi-range (`multipart/byteranges`) responses (planned)

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
Router router;
router.setDefault([](const HttpRequest&){ return HttpResponse(200,"OK").body("GLOBAL"); });
router.setDefault([](const HttpRequest&, HttpResponseWriter& w){ 
  w.status(200,"OK");
  w.contentType("text/plain");
  w.writeBody("STREAMFALLBACK"); 
  w.end(); 
});
router.setPath(http::Method::GET, "/stream", [](const HttpRequest&, HttpResponseWriter& w){ 
  w.status(200,"OK"); 
  w.contentType("text/plain"); 
  w.writeBody("PS"); 
  w.end(); 
});
router.setPath(http::Method::POST, "/stream", [](const HttpRequest&){ return HttpResponse{201, "Created"}.body("NORMAL"); });
```

Behavior:

- GET /stream → path streaming
- POST /stream → path fixed
- GET /other → global streaming fallback
- POST /other → global fixed (since only global fixed + streaming; precedence chooses streaming for GET only)

Testing: `tests/http_streaming_test.cpp` covers precedence, conflicts, HEAD suppression, keep-alive reuse.

### Accessing TLS Metrics

```cpp
SingleHttpServer server;
auto st = server.stats();
log::info("handshakes={} clientCerts={} alpnStrictMismatches={}\n",
          st.tlsHandshakesSucceeded,
          st.tlsClientCertPresent,
          st.tlsAlpnStrictMismatches);
for (const auto& [proto,count] : st.tlsAlpnDistribution) {
  log::info("ALPN {} -> {}\n", proto, count);
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
log::info("server listening on {}", 8080);
```

## OpenTelemetry Integration

Optional (`AERONET_ENABLE_OPENTELEMETRY`). Provides distributed tracing and metrics via OpenTelemetry SDK.

### Architecture

**Instance-based telemetry.** Each `SingleHttpServer` owns its own `TelemetryContext` instance. No global singletons or static state.

Key design principles:

- Per-instance isolation: Multiple servers with independent telemetry configurations
- Explicit lifecycle: Telemetry instance tied to server lifetime
- Error transparency: All telemetry failures logged via `log::error()` (no silent no-ops)

Configuration via `HttpServerConfig::telemetry`:

```cpp
HttpServerConfig cfg;
cfg.withTelemetryConfig(TelemetryConfig{}
                            .withEndpoint("http://localhost:4318")  // OTLP HTTP endpoint base URL
                            .withServiceName("my-service")
                            .withSampleRate(1.0)  // trace sampling rate (0.0 to 1.0)
                            .enableDogStatsDMetrics());  // Optional DogStatsD emission
```

`dogStatsDEnabled` convenience flag plus socket/tag helpers so lightweight DogStatsD
metrics (Unix Domain Socket) can be emitted even when OpenTelemetry support is compiled out. Covered by
`objects/test/opentelemetry-integration_test.cpp`.

### Built-in Instrumentation (phase 1)

Automatic (no handler code changes):

**Traces:**

- `http.request` spans for each HTTP request
- `http.middleware` spans around request (pre) and response (post) middleware execution with attributes capturing
  scope (`aeronet.middleware.scope`), position (`aeronet.middleware.index`), streaming state, short-circuit decisions, and exception
  flags (see `tests/http-routing_test.cpp` for middleware pipeline coverage)
- Attributes: `http.method`, `http.target`, `http.status_code`, `http.request.body.size`, `http.response.body.size`

**Metrics (counters):**

- `aeronet.events.processed` – epoll events processed
- `aeronet.connections.accepted` – new connections
- `aeronet.bytes.read` – bytes read from clients
- `aeronet.bytes.written` – bytes written to clients

**Metrics (histograms):**

**aeronet** exposes a lightweight histogram API via `TelemetryContext::histogram(name, value)`.

Bucket boundaries are configured explicitly in `TelemetryConfig` (OpenTelemetry explicit-bucket histogram view).
This allows you to define stable bucket boundaries for a given instrument name.

Configuration:

- Register boundaries per instrument name via `TelemetryConfig::addHistogramBuckets(name, boundaries)`.
- Boundaries must be finite and **strictly increasing**.

Behavior:

- DogStatsD emission does not use client-side bucket boundaries; histogram aggregation/bucketing is configured
  on the DogStatsD backend/agent.

All instrumentation is fully async (OTLP HTTP exporter) with configurable endpoints and sample rates. When
`dogStatsDEnabled` is enabled, Aeronet also emits counter metrics over DogStatsD/UDS even if the build
does not include OpenTelemetry.

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

## Access-Control (CORS) Helpers

**Opt-in per-route CORS configuration** for web APIs served by aeronet. The CORS implementation is fully RFC-compliant and production-ready.

### Overview

- **Header:** `aeronet/cors-policy.hpp`
- **Core class:** `aeronet::CorsPolicy` — immutable after setup, thread-safe for reuse
- **Integration:** Attach policy to individual routes via `Router::setPath(...).cors(policy)` or set a default policy for all routes via `RouterConfig::withDefaultCorsPolicy(policy)`
- **Automatic preflight:** OPTIONS requests with `Access-Control-Request-Method` header are recognized as preflight and receive automatic 204 No Content responses with appropriate CORS headers
- **Actual request handling:** CORS headers are injected into all matching responses (both buffered `HttpResponse` and streaming `HttpResponseWriter`)

### Key Features

1. **Origin validation:**
   - Wildcard (`*`) for public APIs
   - Exact-match allow-list (case-insensitive, zero-alloc lookup)
   - Automatic origin mirroring when credentials are enabled or specific origins configured

2. **Credentials support:**
   - `allowCredentials(true)` enables `Access-Control-Allow-Credentials: true` and forces specific origin mirroring (never `*`)

3. **Method & header control:**
   - `allowMethods(Method bitmask)` — configures which HTTP methods are allowed for the route
   - `allowRequestHeader(name)` / `allowRequestHeaders({...})` — controls which custom headers clients can send
   - `exposeHeader(...)` — controls which response headers are exposed to client JavaScript

4. **Preflight caching:**
   - `maxAge(duration)` sets `Access-Control-Max-Age` to reduce preflight requests

5. **Private network access:**
   - `allowPrivateNetwork(true)` enables `Access-Control-Allow-Private-Network: true` for local network requests

6. **Vary header handling:**
   - When origin is mirrored (credentials or specific origins), aeronet automatically adds `Vary: Origin` or appends `, Origin` to existing `Vary` header
   - Prevents cache confusion when different origins receive different responses
   - Works correctly for both buffered and streaming responses

### Configuration API

```cpp
CorsPolicy policy;
policy.allowOrigin("https://app.example.com")
      .allowOrigin("https://staging.example.com")
      .allowMethods(http::Method::GET | http::Method::POST | http::Method::PUT)
      .allowRequestHeader("Authorization")
      .allowRequestHeader("X-Custom-Header")
      .exposeHeader("X-Total-Count")
      .exposeHeader("X-Page-Size")
      .allowCredentials(true)
      .maxAge(std::chrono::hours{1});
```

All configuration methods return `CorsPolicy&` for fluent chaining.

### Router Integration

**Per-route policy:**

```cpp
CorsPolicy policy;
Router router;
router.setPath(http::Method::GET | http::Method::POST, "/api/data", 
               [](const HttpRequest& req) { return HttpResponse(200); })
      .cors(std::move(policy));
```

**Default policy for all routes:**

```cpp
CorsPolicy policy;
RouterConfig routerConfig;
routerConfig.withDefaultCorsPolicy(std::move(policy));
SingleHttpServer server(HttpServerConfig{}, routerConfig);
```

**Route-specific override:**
Routes with explicit `.cors(...)` always take precedence over the default policy.

### Behavior Details

#### Preflight Requests

- Recognized when: `OPTIONS` method + `Access-Control-Request-Method` header present
- Response: `204 No Content` with:
  - `Access-Control-Allow-Origin` (mirrored or `*`)
  - `Access-Control-Allow-Methods` (computed from route registration)
  - `Access-Control-Allow-Headers` (echoed from request or from allow-list)
  - `Access-Control-Max-Age` (if configured)
  - `Access-Control-Allow-Credentials` (if enabled)
  - `Vary: Origin` (if origin is mirrored)

#### Actual Requests

- CORS headers added to all responses (both `HttpResponse` and `HttpResponseWriter`)
- Headers injected:
  - `Access-Control-Allow-Origin`
  - `Access-Control-Allow-Credentials` (if enabled)
  - `Access-Control-Expose-Headers` (if configured)
  - `Vary: Origin` (if origin is mirrored)

#### Origin Validation

- Case-insensitive comparison
- Empty origin header → rejected (no CORS headers)
- Origin not in allow-list → rejected (403 Forbidden for preflight, suppressed handler for actual requests)

#### Precedence Rules

1. **Per-route policy** (via `.cors(...)`) — highest priority
2. **Router default policy** (via `RouterConfig::withDefaultCorsPolicy(...)`)
3. **No CORS** — no headers emitted

### Performance Notes

- Zero-allocation origin lookup (case-insensitive interned comparison)
- Precomputed comma-joined header lists
- Single-pass Vary header reconciliation

### CORS Test Coverage

Comprehensive test coverage in `tests/http_options_trace_test.cpp`:

- Preflight handling (success, method/header/origin denial)
- Actual request CORS header injection
- Vary header handling (both buffered and streaming)
- Credentials + specific origins
- Wildcard origins
- Multiple allowed origins
- Private network access

See also: `docs/cors-helpers.md` for extended design notes and implementation details.

### Example: Multi-Origin API

```cpp
CorsPolicy apiCors;
apiCors.allowOrigin("https://app.example.com")
       .allowOrigin("https://mobile.example.com")
       .allowMethods(http::Method::GET | http::Method::POST | http::Method::PUT | http::Method::DELETE)
       .allowRequestHeader("Authorization")
       .allowRequestHeader("Content-Type")
       .exposeHeader("X-Total-Count")
       .exposeHeader("X-RateLimit-Remaining")
       .allowCredentials(true)
       .maxAge(std::chrono::hours{24});

Router router;
router.setPath(http::Method::GET | http::Method::POST, "/api/*", 
               [](const HttpRequest& req) { return HttpResponse(200); })
      .cors(std::move(apiCors));
```

## WebSocket (RFC 6455)

`aeronet` supports WebSocket connections via the HTTP upgrade mechanism per [RFC 6455](https://tools.ietf.org/html/rfc6455).
WebSocket enables full-duplex, bidirectional communication between client and server over a single TCP connection.

### Features

- [x] HTTP/1.1 WebSocket upgrade handshake validation
- [x] Sec-WebSocket-Key / Sec-WebSocket-Accept computation
- [x] Text and Binary frame types
- [x] Continuation frames for message fragmentation
- [x] Control frames: Ping, Pong, Close
- [x] Close handshake with status codes and reasons
- [x] Frame masking (required for client-to-server, rejected if missing)
- [x] Configurable maximum message size
- [x] Protocol subprotocol negotiation (Sec-WebSocket-Protocol)
- [x] permessage-deflate compression (RFC 7692)
- [x] Close timeout with automatic force-close

### Quick Example

Register a WebSocket endpoint using `Router::setWebSocket()`:

```cpp
#include <aeronet/aeronet.hpp>
#include <iostream>
#include <span>

using namespace aeronet;
using namespace aeronet::websocket;

int main() {
  Router router;

  // Register WebSocket endpoint with factory for echo functionality
  router.setWebSocket("/ws", WebSocketEndpoint::WithFactory([](const HttpRequest& /*req*/) {
    auto handler = std::make_unique<WebSocketHandler>();

    // Capture raw pointer before moving handler
    WebSocketHandler* handlerPtr = handler.get();

    handler->setCallbacks(websocket::WebSocketCallbacks{
        .onMessage =
            [handlerPtr](std::span<const std::byte> payload, bool isBinary) {
              // Echo back the message
              if (isBinary) {
                handlerPtr->sendBinary(payload);
              } else {
                handlerPtr->sendText({reinterpret_cast<const char*>(payload.data()), payload.size()});
              }
            },
        .onClose =
            [](CloseCode code, std::string_view reason) {
              std::cout << "Connection closed: " << static_cast<uint16_t>(code)
                        << " - " << reason << "\n";
            },
    });

    return handler;
  }));

  SingleHttpServer server(HttpServerConfig{}.withPort(8080), std::move(router));
  server.run();
}
```

For simpler use cases where you don't need to send messages from callbacks:

```cpp
Router router;
// Simple logging endpoint (no echo, just log incoming messages)
router.setWebSocket("/log", WebSocketEndpoint::WithCallbacks(websocket::WebSocketCallbacks{
    .onMessage = [](std::span<const std::byte> payload, bool isBinary) {
      // Handle message
    },
}));
```

### Upgrade Handshake

When a client sends a WebSocket upgrade request:

```http
GET /ws HTTP/1.1
Host: localhost:8080
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13
```

aeronet validates:

1. HTTP method is GET
2. `Upgrade: websocket` header present
3. `Connection: Upgrade` header present
4. `Sec-WebSocket-Version: 13` header present
5. Valid `Sec-WebSocket-Key` (24-character base64 string)

On success, the server responds with 101 Switching Protocols and transitions the connection to WebSocket mode.

### Frame Types

| Opcode | Type | Description |
|--------|------|-------------|
| 0x0 | Continuation | Continuation of a fragmented message |
| 0x1 | Text | UTF-8 text message |
| 0x2 | Binary | Binary message |
| 0x8 | Close | Connection close request |
| 0x9 | Ping | Heartbeat request |
| 0xA | Pong | Heartbeat response |

### Close Codes

Common WebSocket close status codes:

| Code | Name | Description |
|------|------|-------------|
| 1000 | Normal | Normal closure |
| 1001 | GoingAway | Endpoint going away (e.g., server shutdown) |
| 1002 | ProtocolError | Protocol error detected |
| 1003 | UnsupportedData | Received unsupported data type |
| 1005 | NoStatusReceived | No status code in close frame |
| 1006 | AbnormalClosure | Connection closed abnormally |
| 1007 | InvalidPayload | Invalid frame payload data |
| 1008 | PolicyViolation | Policy violation |
| 1009 | MessageTooBig | Message too large |
| 1011 | InternalError | Server encountered an error |

### WebSocket Configuration

WebSocket behavior can be configured via `WebSocketConfig`:

```cpp
websocket::WebSocketConfig config;
config.maxMessageSize = 16 * 1024 * 1024;  // 16 MB max message
config.maxFrameSize = 1024 * 1024;          // 1 MB max frame
config.closeTimeout = std::chrono::seconds(5);  // 5 second close timeout

// Use config with callbacks
Router router;
router.setWebSocket("/ws", WebSocketEndpoint::WithConfigAndCallbacks(config, websocket::WebSocketCallbacks{}));
```

#### Close Timeout

When a close frame is sent, the connection enters the `CloseSent` state and waits for the peer's close response. If the peer doesn't respond within `closeTimeout`, you can force-close the connection:

```cpp
websocket::WebSocketHandler handler;
if (handler.hasCloseTimedOut()) {
  handler.forceCloseOnTimeout();  // Transitions to Closed state
}
```

### permessage-deflate Compression (RFC 7692)

`aeronet` supports WebSocket compression via the `permessage-deflate` extension per [RFC 7692](https://tools.ietf.org/html/rfc7692). This significantly reduces bandwidth for text-heavy payloads.

#### Enabling Compression

Compression is automatically negotiated during the WebSocket handshake when the client offers `Sec-WebSocket-Extensions: permessage-deflate`. Configure compression behavior via `DeflateConfig`:

```cpp
websocket::DeflateConfig deflateConfig;
deflateConfig.serverMaxWindowBits = 15;          // Server LZ77 window size (9-15)
deflateConfig.clientMaxWindowBits = 15;          // Client LZ77 window size (9-15)
deflateConfig.serverNoContextTakeover = false;   // Reuse compression context
deflateConfig.clientNoContextTakeover = false;   // Reuse decompression context
deflateConfig.minCompressSize = 64;              // Don't compress messages < 64 bytes
deflateConfig.compressionLevel = 6;              // zlib compression level (1-9)

websocket::WebSocketConfig config;
config.deflateConfig = deflateConfig;

Router router;
router.setWebSocket("/ws", 
                    WebSocketEndpoint::WithConfigAndCallbacks(config,
                                                              websocket::WebSocketCallbacks{
                                                                .onMessage =
                                                                    [](std::span<const std::byte> payload, bool isBinary) {
                                                                      // TODO: handle message
                                                                    },
                                                                .onPing = {},
                                                                .onPong = {},
                                                                .onClose = {},
                                                                .onError = {},
                                                              }));
```

#### Negotiation Parameters

| Parameter | Description |
|-----------|-------------|
| `server_max_window_bits` | Maximum LZ77 window size (log2) the server will use |
| `client_max_window_bits` | Maximum LZ77 window size (log2) the client will use |
| `server_no_context_takeover` | Server resets compression context after each message |
| `client_no_context_takeover` | Client resets compression context after each message |

When negotiation succeeds, messages are automatically compressed/decompressed transparently—callbacks receive uncompressed payloads.

### Thread Safety

WebSocket handlers run on the same reactor thread as HTTP handlers. The `WebSocketHandler` pointer captured in callbacks is valid only during callback execution. For async operations, capture handler data (not the handler pointer) and use thread-safe mechanisms to communicate back.

## HTTP/2 (RFC 9113)

`aeronet` provides optional HTTP/2 support implementing [RFC 9113](https://httpwg.org/specs/rfc9113.html) with HPACK header compression ([RFC 7541](https://httpwg.org/specs/rfc7541.html)).

### Feature Matrix

| Feature | Status | Notes |
|---------|--------|-------|
| HPACK header compression | ✔ | Static/dynamic table, Huffman encoding |
| Stream multiplexing | ✔ | Multiple concurrent streams per connection |
| Flow control | ✔ | Per-stream and connection-level |
| ALPN "h2" negotiation | ✔ | Over TLS (requires OpenSSL) |
| h2c (cleartext prior knowledge) | ✔ | Client sends HTTP/2 preface directly |
| h2c upgrade (HTTP/1.1 → HTTP/2) | ✔ | Via `Upgrade: h2c` header |
| Server push | ✗ | Disabled (rarely used by modern clients) |
| PRIORITY frames | ✔ | Optional, configurable |

Additional notes

- Static file responses created via `HttpResponse::file(...)` (used by `StaticFileHandler`) are serialized as HTTP/2 DATA frames by reading the file in bounded chunks.
- The implementation is flow-control aware: it sends up to the available connection/stream window and continues after receiving `WINDOW_UPDATE` frames (no full in-memory file load).
- Tests: see `tests/http-tls-io_test.cpp` (`HttpRangeStatic_H2Tls.LargeFileStreaming_H2Tls`).

### Enabling HTTP/2

HTTP/2 support is controlled at build time via CMake:

```bash
cmake -S . -B build -DAERONET_ENABLE_HTTP2=ON
cmake --build build
```

When `AERONET_ENABLE_HTTP2` is OFF, the `aeronet/http2` module is not compiled and the HTTP/2-specific API surface (such as `HttpServerConfig::withHttp2()` / `enableHttp2()`) is not available.

When enabled, you configure HTTP/2 through `Http2Config` on your server:

```cpp
#include <aeronet/aeronet.hpp>

using namespace aeronet;

int main() {
  Router router;
  
  // Unified handler for both HTTP/1.1 and HTTP/2
  // Use req.isHttp2() and req.streamId() to detect HTTP/2 if needed
  router.setDefault([](const HttpRequest& req) {
    if (req.isHttp2()) {
      return HttpResponse(200).body("Hello from HTTP/2! Stream " + std::to_string(req.streamId()) + "\n");
    }
    return HttpResponse(200).body("Hello from HTTP/1.1\n");
  });

  // HTTP/2 configuration
  Http2Config http2Config;
  http2Config.enable = true;
  http2Config.maxConcurrentStreams = 100;
  http2Config.initialWindowSize = 65535;

  // Configure server with TLS, ALPN, and HTTP/2
  HttpServerConfig config;
  config.withPort(8443)
      .withTlsCertKey("server.crt", "server.key")
      .withTlsAlpnProtocols({"h2", "http/1.1"})  // Prefer HTTP/2
      .withHttp2(http2Config);

  SingleHttpServer server(std::move(config), std::move(router));
  server.run();
}
```

### Unified Handler API

HTTP/2 requests use the same `HttpRequest` type and handlers as HTTP/1.1. The framework automatically routes requests to your handlers regardless of protocol version. To detect HTTP/2 in your handler:

```cpp
Router router;

// Single handler works for both HTTP/1.1 and HTTP/2
router.setDefault([](const HttpRequest& req) {
  if (req.isHttp2()) {
    // HTTP/2-specific logic using req.streamId(), req.scheme(), etc.
    return HttpResponse(200).body("HTTP/2 stream " + std::to_string(req.streamId()) + "\n");
  }
  return HttpResponse(200).body("HTTP/1.1 response\n");
});

// Per-path handlers work identically for both protocols
router.setPath(http::Method::GET, "/api/{resource}", [](const HttpRequest& req) {
  return HttpResponse(200).body("Resource: " + std::string(req.pathParams().at("resource")) + "\n");
});

HttpServerConfig config;
config.withPort(8080)
    .withHttp2(Http2Config{.enable = true, .enableH2c = true});

SingleHttpServer server(std::move(config), std::move(router));
server.run();
```

Notes:

- Pattern syntax, trailing-slash policy, and HEAD→GET fallback apply identically to both protocols.
- `AsyncRequestHandler` and `StreamingHandler` are also supported for HTTP/2 (async handlers execute synchronously since HTTP/2 streams are already multiplexed).
- All handler types registered in the Router work transparently for both HTTP/1.1 and HTTP/2.

### Http2Config

The `Http2Config` structure provides comprehensive HTTP/2 tuning.

### ALPN Protocol Negotiation (h2)

For TLS connections, HTTP/2 is negotiated via ALPN (Application-Layer Protocol Negotiation):

```cpp
HttpServerConfig config;
config.withTlsCertKey("server.crt", "server.key")
    .withTlsAlpnProtocols({"h2", "http/1.1"});  // Server advertises both

// After TLS handshake, if client selected "h2":
// - Connection automatically switches to HTTP/2 protocol handler
// - All subsequent frames use HTTP/2 binary framing
```

The server automatically detects the negotiated protocol and routes the connection to the appropriate handler.

### Cleartext HTTP/2 (h2c)

HTTP/2 over cleartext (without TLS) is supported via two mechanisms.

#### Prior Knowledge

Client sends the HTTP/2 connection preface (`PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`) directly:

```cpp
Http2Config config;
config.enableH2c = true;  // Accept direct HTTP/2 preface on plaintext
```

#### HTTP/1.1 Upgrade

Client sends an HTTP/1.1 request with upgrade headers:

```text
GET / HTTP/1.1
Host: localhost
Connection: Upgrade, HTTP2-Settings
Upgrade: h2c
HTTP2-Settings: AAMAAABkAAQBAAAAAAIAAAAA
```

The server responds with `101 Switching Protocols` and transitions to HTTP/2:

```cpp
Http2Config config;
config.enableH2cUpgrade = true;  // Enable Upgrade mechanism
```

### Testing HTTP/2

Test with curl:

```bash
# HTTPS with ALPN negotiation
curl -k --http2 https://localhost:8443/hello

# h2c (cleartext) with prior knowledge
curl --http2-prior-knowledge http://localhost:8080/hello

# h2c via upgrade
curl --http2 http://localhost:8080/hello
```

### Thread Safety

HTTP/2 handlers execute on the same reactor thread as HTTP/1.1. The `HttpRequest` references are valid only during handler execution. For long-running operations, copy any needed data before returning.

## Future Expansions

Planned / potential: TLS hot reload & SNI, richer logging & metrics, additional OpenTelemetry instrumentation (histograms, gauges).

- [ ] Additional OpenTelemetry instrumentation (histograms, gauges)
