# aeronet Roadmap — Planned / Not Implemented

This file lists planned features and near-term priorities. Implemented features are documented in `docs/FEATURES.md` and `README.md` (notably: zero-copy `sendfile()` support and the lightweight `sendFile` helper are implemented in v0.4.x).

## High priority

- Range & Conditional Requests (RFC 7233, RFC 7232)
  - Single-range 206 Partial Content support, `Content-Range` header, `Accept-Ranges: bytes`.
  - `If-Range`, `If-Modified-Since`, `If-None-Match` / `ETag` handling and 304 responses.
  - 416 (Range Not Satisfiable) handling.
  - Integration with `sendFile` offsets/lengths so range serving is zero-copy on plaintext sockets.
- KTLS integration (kernel TLS)
  - Optional kernel TLS support to enable encrypted zero-copy sendfile on platforms that support it.
  - Runtime detection + OpenSSL integration, graceful fallback to existing TLS write path when unavailable.
- Static-file helper & directory index
  - High-level, safe handler for serving filesystem trees: path sanitization, MIME detection, ETag/Last-Modified generation, directory index rendering, and automatic range & conditional handling.

## Medium priority

- Structured logging / pluggable sinks
- Enhanced parser diagnostics (byte offset in parse errors)
- Middleware helpers (lightweight routing/middleware layer)
- Access-Control (CORS) helpers
- Multipart / multiple-range responses (`multipart/byteranges`) support (RFC 7233 multi-range)
- Performance benchmarking & CI perf tests (microbenchmarks for sendfile vs TLS fallback)

## Long-term / Nice-to-have

- Fuzz harness integration (libFuzzer targets)
- HTTP/2 prototype (leveraging ALPN + transport abstraction)
- OCSP stapling / advanced TLS features

### TLS enhancements (detailed roadmap)

#### Phase 2

- Session resumption (tickets) + resumed/full handshake counters
- SNI multi-cert routing (single listener, map host -> cert bundle)
- Failure reason counters + structured handshake event callback
- Cipher policy presets & disable compression flag
- Handshake concurrency limit + basic rate limiting
- MultiHttpServer shared ticket key support

#### Phase 2.5

- Hot cert/key reload (atomic swap)
- Key algorithm diversity tests (RSA + ECDSA)
- Dynamic trust store update API

#### Phase 3 (Advanced / Enterprise)

- OCSP stapling (passive, cached)
- Optional CRL / revocation hooks
- Histogram / percentile metrics
- Key log (debug only)
- Security hardening audits (zeroization, memory scrub confirmations)

#### Phase 4 (Future Protocol / Extensibility)

- ALPN "h2" groundwork (without committing to HTTP/2 yet)
- Per-SNI mTLS policies
- Session ticket key rotation scheduling & multi-key window
- (Stretch) Exploring QUIC/HTTP/3 (would likely be a separate transport layer, so only mention if strategic)
