# aeronet Roadmap — Planned / Not Implemented

This file lists planned features that are not yet implemented. For implemented features, see `docs/FEATURES.md` and `README.md`.

## High priority

- Trailer header exposure (incoming chunked trailers)
- Zero-copy sendfile() support / static file helper

## Medium priority

- Structured logging / pluggable sinks
- Enhanced parser diagnostics (byte offset in parse errors)
- Middleware helpers (lightweight routing/middleware layer)
- Access-Control (CORS) helpers
- Range / Conditional Requests (References: RFC 7233 (Range), RFC 7232 (Conditional Requests))
  No support yet for Range, If-Modified-Since, If-None-Match, etc. Not mandatory but important for real web servers (streaming, resume, caching).

## Long-term / Nice-to-have

- Fuzz harness integration (libFuzzer targets)
- HTTP/2 prototype (leveraging ALPN + transport abstraction)
- OCSP stapling / advanced TLS features

### TLS enhancements (detailed roadmap)

### Phase 2

- Session resumption (tickets) + resumed/full handshake counters
- SNI multi-cert routing (single listener, map host → cert bundle)
- Failure reason counters + structured handshake event callback
- Cipher policy presets & disable compression flag
- Handshake concurrency limit + basic rate limiting
- MultiHttpServer shared ticket key support

### Phase 2.5

- Hot cert/key reload (atomic swap)
- Key algorithm diversity tests (RSA + ECDSA)
- Dynamic trust store update API

### Phase 3 (Advanced / Enterprise)

- OCSP stapling (passive, cached)
- Optional CRL / revocation hooks
- Histogram / percentile metrics
- Key log (debug only)
- Security hardening audits (zeroization, memory scrub confirmations)

### Phase 4 (Future Protocol / Extensibility)

- ALPN “h2” groundwork (without committing to HTTP/2 yet)
- Per-SNI mTLS policies
- Session ticket key rotation scheduling & multi-key window
- (Stretch) Exploring QUIC/HTTP/3 (would likely be a separate transport layer, so only mention if strategic)
