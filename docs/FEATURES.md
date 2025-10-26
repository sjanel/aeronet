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
1. [Query String & Parameters](#query-string--parameters)
1. [Trailing Slash Policy](#trailing-slash-policy)
1. [Construction Model (RAII & Ephemeral Ports)](#construction-model-raii--ephemeral-ports)
1. [MultiHttpServer Lifecycle](#multihttpserver-lifecycle)
1. [Built-in Kubernetes-style probes](#built-in-kubernetes-style-probes)
1. [TLS Features](#tls-features)
1. [CONNECT (HTTP tunneling)](#connect-http-tunneling)
1. [Streaming Responses](#streaming-responses-chunked--incremental)
1. [Static File Handler (RFC 7233 / RFC 7232)](#static-file-handler-rfc-7233--rfc-7232)
1. [Mixed Mode Dispatch Precedence](#mixed-mode--dispatch-precedence)
1. [Logging](#logging)
1. [OpenTelemetry Integration](#opentelemetry-integration)
1. [Future Expansions](#future-expansions)
1. [Large-body optimization](#large-body-optimization)

## HTTP/1.1 Feature Matrix

Legend: [x] implemented, [ ] planned / not yet.

### Core HTTP parsing & routing

- [x] Request line parsing (method, target, version)
- [x] Header field parsing (no folding / continuations)
- [x] Case-insensitive header lookup helper
- [x] Router path matching and allowed-method computation
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
- [ ] Multipart/form-data convenience utilities

Where to look: see "Inbound Request Decompression (Config Details)" for decompression behavior and the parser docs for chunked/CL handling.

### Response generation & streaming

- [x] Basic fixed body responses
- [x] HEAD method (suppressed body, correct Content-Length)
- [x] Outgoing chunked / streaming responses (basic API: status/headers + incremental write + end, keep-alive capable)
- [x] Outbound trailer headers (buffered via HttpResponse::addTrailer, streaming via HttpResponseWriter::addTrailer)
- [x] Mixed-mode dispatch (simultaneous registration of streaming and fixed handlers with precedence)
- [x] Compression (gzip & deflate) (phase 1: zlib) – streaming + buffered with threshold & q-values

Where to look: see the "Compression & Negotiation" section for full details and configuration.

### Methods & special semantics

- [x] OPTIONS * handling (returns an Allow header per RFC 7231 §4.3)
- [x] TRACE method support (echo) — optional and configurable via `HttpServerConfig::TracePolicy`

Where to look: see the "OPTIONS & TRACE behavior" subsection below.

### Status & error handling

- [x] 400 Bad Request (parse errors, CL+TE conflict)
- [x] 400 on HTTP/1.0 requests carrying Transfer-Encoding
- [x] 405 Method Not Allowed (enforced when path exists but method not in allow set)
- [x] 413 Payload Too Large (body limit)
- [x] 415 Unsupported Media Type (content-encoding based)
- [ ] 415 Unsupported Media Type (content-type based)
- [x] 431 Request Header Fields Too Large (header limit)
- [x] 501 Not Implemented (unsupported Transfer-Encoding)
- [x] 505 HTTP Version Not Supported
  
  Note: aeronet already maps unknown request `Content-Encoding` values to **415** when the inbound
  decompression feature is enabled (see "Inbound Request Decompression"). However, automatic
  `Content-Type` (media-type) validation is intentionally left to application code. If you need
  global Content-Type enforcement, implement a small validator middleware or configure your handlers
  to check the `Content-Type` header and return **415** when appropriate.

Where to look: see the "Status & error handling" notes and parser error descriptions below.

### Headers & protocol niceties

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
- [x] Multi-instance orchestration wrapper (`MultiHttpServer`) (explicit `reusePort=true` for >1 threads; aggregated stats; resolved port immediately after construction)
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
std::string big = generate_large_payload();
return HttpResponse(200, "OK").contentType("application/octet-stream").body(std::move(big));

// Move a vector<char>
std::vector<char> v = read_file_bytes(path);
return HttpResponse(200, "OK").contentType("application/octet-stream").body(std::move(v));

// Move a unique_ptr<char[]> for raw blob ownership
std::unique_ptr<char[]> blob = load_blob();
std::size_t blobSize = /* known size */;
return HttpResponse(200, "OK").contentType("application/octet-stream").body(std::move(blob), blobSize);
```

These patterns hand ownership to the server without duplicating the payload, enabling efficient zero-copy handoff
for large responses.

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
server.router().setPath(http::Method::GET, "/upload", [](const HttpRequest& req) {
  // Access request body
  std::string body = req.body();
  
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
server.router().setPath(http::Method::GET, "/data", [](const HttpRequest& req) {
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
return HttpResponse(200)
    .body("data")
    .addTrailer("X-Checksum", "xyz")
    .addTrailer("X-Signature", "sig123");
```

#### Streaming Response Trailers (HttpResponseWriter)

For chunked/streaming responses, use `HttpResponseWriter::addTrailer()`:

```cpp
server.router().setPath(http::Method::GET, "/stream",
    [](const HttpRequest& req, HttpResponseWriter& w) {
  w.statusCode(200);
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
- Final chunk string is moved into HttpBody for efficient transmission

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

`HttpServer` exposes a lifecycle state machine to coordinate shutdown:

| State | Description | Entered via |
|-------|-------------|-------------|
| Idle | Listener closed, loop inactive | Default / after drain/stop |
| Running | Event loop servicing connections | `run()` / `runUntil()` |
| Draining | Listener closed; existing connections finish with `Connection: close` | `beginDrain()` |
| Stopping | Immediate teardown, pending connections closed | `stop()` or fatal epoll error |

Key API points:

- **`beginDrain(std::chrono::milliseconds maxWait = 0)`** stops accepting new connections, keeps existing keep-alive sessions long enough to finish their current response, and injects `Connection: close` so the client does not reuse the socket. When `maxWait` is non-zero, a deadline is armed; any connections still open when it expires are closed immediately. Calling `beginDrain()` again with a shorter timeout shrinks the deadline.
- **`isDraining()`** reflects whether the server is currently in the draining state. `isRunning()` still reports `true` until the drain completes or a stop occurs.
- **Wrappers** — `AsyncHttpServer::beginDrain()` / `isDraining()` and `MultiHttpServer::beginDrain()` / `isDraining()` forward to the underlying `HttpServer` instances, enabling the same graceful drain flow when the server runs on background threads or across multiple reactors.
- Draining is restart-friendly: once all connections are gone (or the deadline forces closure) the lifecycle resets to `Idle` and the server can be started again with another `run()`.
- `stop()` remains the immediate shutdown primitive; it transitions to `Stopping`, force-closes all connections and wakes the event loop right away.

This drain lifecycle allows supervisors to quiesce traffic (e.g., removing an instance from load balancers) while letting outstanding requests complete and optionally bounding the wait for stubborn clients.

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
  - `AsyncHttpServer::beginDrain()` and `MultiHttpServer::beginDrain()` forward to their underlying `HttpServer` instances so the same graceful behavior is available for background or multi‑reactor setups. `stop()` continues to request immediate termination on wrappers as before.

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

## MultiHttpServer lifecycle

Manages N reactors via `SO_REUSEPORT`.

### In a nutshell

- Constructor binds & resolves port (ephemeral resolved once).
- Restart rebuilds underlying single‑shot servers; same port reused.
- Modify handlers only while stopped (between stop/start).
- `reusePort=true` required for `threadCount > 1`.
- Movable even while running (vector storage stable).
- Graceful drain propagates: `beginDrain(maxWait)` stops all accept loops, existing keep-alive connections receive `Connection: close`, and `isDraining()` reports when any underlying instance is still draining.

### MultiHttpServer restart example

```cpp
HttpServerConfig cfg; cfg.port=0; cfg.reusePort=true; MultiHttpServer multi(cfg,4);
multi.router().setDefault([](const HttpRequest&){ return HttpResponse(200,"OK").contentType("text/plain").body("hi\n"); });
multi.start(); multi.stop(); multi.start();
```

## Built-in Kubernetes-style probes

Aeronet can optionally provide a small set of built-in HTTP probe endpoints intended to be used
by Kubernetes-style health checks and load-balancers. These probes are lightweight, handled
entirely by the server, and do not require application handlers to be installed when enabled.

### Probes in a nutshell

- Enabled via `HttpServerConfig::withBuiltinProbes(BuiltinProbesConfig)` or `enableBuiltinProbes(true)`.
- Default probe paths (configurable in `BuiltinProbesConfig`):
  - Liveness: `/livez` — indicates the process has started (HTTP 200 when started)
  - Readiness: `/readyz` — indicates the server is ready to receive new requests (HTTP 200)
  - Startup: `/startupz` — reports startup progress (returns 503 until the server has fully started)
- The probe handlers return minimal responses (status only, configurable Content-Type) and avoid heavy work.

### Probes lifecycle semantics

- `liveness` (livez): reflects a simple "started" flag. Once the server has successfully entered the
  running state this endpoint returns 200. It is intended to indicate the process is alive.
- `readiness` (readyz): reflects a `ready` flag that is cleared early during graceful shutdown (`beginDrain()`).
  During normal operation it returns 200. When `beginDrain()` is invoked the server sets `ready=false`
  (returning 503) so load-balancers and Kubernetes can stop sending new connections while existing
  keep-alive requests drain.
- `startup` (startupz): returns 503 prior to the server fully initializing and starts returning 200 once
  initialization completes. This is useful for probes that should fail until the server is truly ready.

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
HttpServer server(std::move(cfg));
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

Testing: see `tests/http_streaming.cpp`.

- [x] `StaticFileHandler` serves directory trees with zero-copy `file`
- [x] RFC 7233 single-range parsing and validation (`Range`, `If-Range`)
- [x] RFC 7232 validators (`If-None-Match`, `If-Match`, `If-Modified-Since`, `If-Unmodified-Since`)
- [x] Strong ETag generation (`size-lastWriteTime`), `Last-Modified`, `Accept-Ranges: bytes`
- [x] 416 (Range Not Satisfiable) with `Content-Range: bytes */N`
- [x] Integration hooks in `HttpServerConfig::staticFiles`

## Static File Handler (RFC 7233 / RFC 7232)

`StaticFileHandler` provides a hardened helper for serving filesystem trees while respecting HTTP caching and range
semantics. The handler is designed to plug into the existing routing API: it is an invocable object that accepts an
`HttpRequest` and returns an `HttpResponse`, so it works with `HttpServer`, `AsyncHttpServer`, and
`MultiHttpServer` exactly like any other handler.

- **Zero-copy transfers**: regular GET requests use `HttpResponse::file()` so plaintext sockets reuse the kernel
  `sendfile(2)` path. TLS endpoints automatically fall back to the buffered write path that aeronet already uses for
  file responses.
- **Single-range support**: `Range: bytes=N-M` (RFC 7233 §2.1) is parsed with strict validation. Valid ranges return
  `206 Partial Content` with `Content-Range`. Invalid syntax returns `416` with `Content-Range: bytes */<size>` per the
  spec. Multi-range requests (comma-separated) are rejected as invalid.
- **Conditional requests**: `If-None-Match`, `If-Match`, `If-Modified-Since`, `If-Unmodified-Since`, and `If-Range`
  are honoured using strong validators. Requests that do not modify the resource return `304 Not Modified` for GET/HEAD
  or `412 Precondition Failed` for unsafe methods. `If-Range` transparently falls back to the full body when the
  validator mismatches.
- **Headers**: the handler always emits `Accept-Ranges: bytes` so clients learn range capability. `ETag` and
  `Last-Modified` are enabled by default (configurable) and share the same strong validator used by conditionals.
- **Safety**: all request paths are normalised under the configured root; `..` segments are rejected. Default index
  fallback (e.g. `index.html`) is configurable or can be disabled.
- **Config entry point**: the immutable configuration lives in `StaticFileConfig`. The handler constructor also accepts a config directly.

Example usage:

```cpp
#include <aeronet/static-file-handler.hpp>

using namespace aeronet;

int main() {
  HttpServerConfig cfg;
  cfg.withPort(8080);

  StaticFileConfig staticFileConfig;
  staticFileConfig.enableRange = true;
  staticFileConfig.addEtag = true;
  staticFileConfig.defaultIndex = "index.html";

  HttpServer server(cfg);
  StaticFileHandler assets("/var/www/html", std::move(staticFileConfig));
  server.router().setPath(http::Method::GET, "/", [assets](const HttpRequest& req) mutable {
    return assets(req);
  });
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
server.router().setDefault([](const HttpRequest&){ return HttpResponse(200,"OK").body("GLOBAL").contentType("text/plain"); });
server.router().setDefault([](const HttpRequest&, HttpResponseWriter& w){ w.setStatus(200,"OK"); w.setHeader("Content-Type","text/plain"); w.write("STREAMFALLBACK"); w.end(); });
server.router().setPath(http::Method::GET, "/stream", [](const HttpRequest&, HttpResponseWriter& w){ w.setStatus(200,"OK"); w.setHeader("Content-Type","text/plain"); w.write("PS"); w.end(); });
server.router().setPath(http::Method::POST, "/stream", [](const HttpRequest&){ return HttpResponse{201, "Created", "text/plain", "NORMAL"}; });
```

Behavior:

- GET /stream → path streaming
- POST /stream → path fixed
- GET /other → global streaming fallback
- POST /other → global fixed (since only global fixed + streaming; precedence chooses streaming for GET only)

Testing: `tests/http_streaming_test.cpp` covers precedence, conflicts, HEAD suppression, keep-alive reuse.

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
