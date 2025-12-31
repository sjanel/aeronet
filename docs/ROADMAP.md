# aeronet Roadmap — Planned / Not Implemented

This file lists planned features and near-term priorities. Implemented features are documented in `docs/FEATURES.md` and `README.md` (notably: zero-copy `sendfile()` support and the lightweight `file` helper are implemented in v0.4.x).

## High priority

- _(none – recent milestones addressed)_

## Medium priority

- Structured logging / pluggable sinks
- Enhanced parser diagnostics (byte offset in parse errors)
- Middleware helpers (lightweight routing/middleware layer)
- Multipart / multiple-range responses (`multipart/byteranges`) support (RFC 7233 multi-range)
- Performance benchmarking & CI perf tests (microbenchmarks for sendfile vs TLS fallback)

### Performance improvement ideas

- Reduce syscalls via batching (`writev`), response buffering strategy, header/body coalescing.
- Use kernel helpers where appropriate: `sendfile`, `TCP_CORK` / `TCP_NOPUSH` to coalesce.
- Enforce backpressure correctness to avoid overload and wasted work.
- Focus on cache locality in hot paths; measure before/after.
- `MSG_ZEROCOPY` for large payloads on Linux (requires fallback path).

## Long-term / Nice-to-have

- Fuzz harness integration (libFuzzer targets)
- OCSP stapling / advanced TLS features

## Recently Completed

- **HTTP/2** (RFC 9113): HPACK header compression, stream multiplexing, flow control, ALPN "h2" negotiation, h2c cleartext (prior knowledge + HTTP/1.1 Upgrade). See [HTTP/2 documentation](FEATURES.md#http2-rfc-9113).

### TLS enhancements (detailed roadmap)

#### Phase 3 (Advanced / Enterprise)

- OCSP stapling (passive, cached)
- Optional CRL / revocation hooks
- Histogram / percentile metrics
- Key log (debug only)
- Security hardening audits (zeroization, memory scrub confirmations)

#### Phase 4 (Future Protocol / Extensibility)

- ~~ALPN "h2" groundwork~~ ✔ (HTTP/2 implemented)
- Per-SNI mTLS policies
- Session ticket key rotation scheduling & multi-key window
- (Stretch) Exploring QUIC/HTTP/3 (would likely be a separate transport layer, so only mention if strategic)
