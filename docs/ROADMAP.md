# aeronet Roadmap & Feature TODOs

This document tracks planned and recently implemented features, grouped by priority. It complements the README feature matrix and expands on design intent.

Legend:

- ✅ Done / implemented
- 🛠 In progress / scheduled next
- 🧪 Experimental / prototype phase
- ⏳ Planned / not started
- ❄ Deferred / low priority

## High Priority (Core HTTP & Robustness)

| Status | Feature | Summary | Key Design Points | Dependencies |
|--------|---------|---------|-------------------|--------------|
| ✅ | Partial Write Buffering & Backpressure | Handle partial socket writes safely | Implemented: per-connection outBuffer + EPOLLOUT re-arm + high-water tracking | None |
| ✅ | Streaming / Outgoing Chunked Responses | Send dynamic data without pre-buffering | HttpResponseWriter API, auto chunked if no length; unified outbound buffer reuse | Write buffering |
| ✅ | Mixed-Mode Streaming + Normal Precedence | Coexist path/global streaming & normal handlers with deterministic priority | Precedence: path streaming > path normal > global streaming > global normal; implicit HEAD->GET; conflict detection | Streaming base |
| ✅ | Header Read Timeout (Slowloris) | Abort very slow header arrivals | Implemented: per-connection headerStart timestamp + enforced close if exceeded (no 408 yet) | None |
| 🛠 | Request Metrics Hook | Expose per-request stats | Initial scaffold: RequestMetrics callback (method, target, status, bytesIn, duration, reused) | None |
| ⏳ | Zero-Copy sendfile() Support | Efficient static file responses | Fallback to buffered if partials, track offset | Write buffering |

## Medium Priority (Features & Developer Experience)

| Status | Feature | Summary | Notes |
|--------|---------|---------|-------|
| ⏳ | Trailer Header Parsing (Incoming) | Parse and expose chunked trailers | Size limit + map storage |
| ⏳ | Structured Logging Interface | Pluggable logger levels / sinks | Fallback logger implemented; need abstraction layer |
| ⏳ | Enhanced Logging (trace IDs) | Optional request correlation id injection | Depends on structured logging |
| ✅ | Compression (gzip & deflate phase 1) | Negotiation (q-values), threshold, streaming + buffered, opt-out | zlib optional build flag |
| ✅ | Compression (brotli) | Higher-ratio alternative (buffered + streaming; negotiation, multi-layer inbound) | Feature flag `AERONET_ENABLE_BROTLI`; context reuse planned |
| ✅ | Compression (zstd) | Balanced speed/ratio (buffered + streaming; negotiation, multi-layer inbound) | Flag `AERONET_ENABLE_ZSTD`; future: dict support |
| ⏳ | Graceful Draining Mode | Stop accepting new connections; finish in-flight | server.beginDrain() + state flag |
| ⏳ | Enhanced Parser Diagnostics (offset) | Provide error location info | Extend callback signature |

## Longer Term / Advanced

| Status | Feature | Summary | Notes |
|--------|---------|---------|-------|
| ✅ | TLS Termination (OpenSSL) - Phase 1 | Optional build, basic handshake + GET test | Isolated module, transport abstraction in place |
| ✅ | TLS Metrics & Observability (Phase 1.5) | Cipher/version, ALPN selection, mismatch counter, handshake logging | Per-server metrics (no globals) |
| ✅ | TLS Handshake Enhancements | Min/max version, handshake timeout, graceful shutdown | New config builders + duration metrics |
| ⏳ | Fuzz Harness Integration | libFuzzer target for parser | Build optional, sanitizer flags |
| ✅ | Outgoing Streaming Compression | Integrated delayed header emission, threshold activation | Depends on encoder abstraction |
| ⏳ | Routing / Middleware Layer | Lightweight dispatch helpers | Optional layer atop core |

## Completed Recently

| Feature | Notes |
|---------|-------|
| Partial Write Buffering & Backpressure | Outbound queue + EPOLLOUT driven flushing + stats |
| Streaming / Outgoing Chunked Responses | HttpResponseWriter + unified buffer path; keep-alive capable |
| Mixed-Mode Streaming Dispatch & HEAD Fallback | Deterministic precedence, conflict prevention, implicit HEAD->GET, tests in `http_streaming_mixed.cpp` |
| Header Read Timeout | Slowloris mitigation via configurable `headerReadTimeout`, connection closed on exceed |
| MultiHttpServer Wrapper | Horizontal scaling orchestration, ephemeral port resolution, aggregated stats |
| Lightweight Logging Fallback | spdlog-style API, ISO8601 timestamps, formatting fallback |
| RFC7231 Date Tests & Caching Validation | Format stability + boundary refresh tests |
| Request Processing Refactor | Split monolithic loop into cohesive helpers |
| Modular Decomposition (parser/response/connection) | Extracted from monolithic `server.cpp` into focused TUs |
| RAII Listening Constructor | Constructor performs bind/listen/epoll add; removed `setupListener()` |
| Chunked Decoding Fuzz (Deterministic) | Random chunk framing test with seed |
| HEAD Max Requests Test | Ensures keep-alive request cap applies to HEAD |
| Malformed & Limit Tests | 400 / 431 / 413 / 505 coverage |
| Percent-Decoding of Request Target | UTF-8 path decoding, '+' preserved, invalid sequences -> 400, tests in `http_url_decoding.cpp` |

## Proposed Ordering (Next Pass)

1. Header Read Timeout (Slowloris mitigation)
2. Metrics Hook (per-request instrumentation)
3. Compression (gzip & deflate) negotiation + streaming integration (DONE)
4. Trailer Parsing (incoming chunked trailers)
5. Draining Mode (graceful connection wind-down)
6. Zero-copy sendfile() support (static files)
7. Enhanced Parser Diagnostics (byte offset)
8. Structured Logging Interface (pluggable sinks / structured fields)
9. (Delivered) brotli compression + streaming integration
10. TLS termination (OpenSSL minimal)
11. Fuzz Harness (extended corpus / sanitizer CI)

## Design Sketches

### HttpResponseWriter (Streaming)

```cpp
class HttpResponseWriter {
public:
  void setStatus(http::StatusCode code, std::string_view reason = {});
  void addCustomHeader(std::string_view name, std::string_view value);
  void setContentLength(std::size_t bytes); // optional explicit length
  void beginChunked();                       // switch to chunked transfer
  bool write(std::string_view chunk);        // returns false if output buffer full
  void end();                                // finalize (send last-chunk if chunked)
};
```

Handler variant:

```cpp
server.setStreamingHandler([](const HttpRequest& req, HttpResponseWriter& w){
  w.setStatus(200, "OK");
  w.addCustomHeader("Content-Type", "text/plain");
  w.beginChunked();
  for (auto piece : pieces) w.write(piece);
  w.end();
});
```

### Metrics Callback

```cpp
struct RequestMetrics {
  std::string_view method;
  std::string_view target;
  int status;
  uint64_t bytesIn;
  uint64_t bytesOut;
  std::chrono::nanoseconds duration;
  bool reusedConnection;
};
server.setMetricsCallback([](const RequestMetrics& m){ /* export */ });
```

### Backpressure Model

- Attempt writev; if partial, store remaining (flatten into buffer or keep iovec queue).
- Register EPOLLOUT; on writable, continue draining.
- While pending output exists, do not read/parse new requests (fairness + ordering).
- Configurable cap: `withMaxWriteBufferBytes(size_t)`; exceed -> connection marked to close after flush (current behavior) or future configurable strategy (e.g., 503).

### Compression Design Notes (Implemented Phase 1 & Planned Extensions)

Phased approach:

Phase 1 implemented:

1. Negotiation: `Accept-Encoding` with q-values; gzip & deflate (zlib) supported; highest q chosen (server preference breaks ties); identity fallback.
2. Threshold & Eligibility: `CompressionConfig::minBytes` gate; streaming buffers until threshold then activates; fixed responses decide immediately.
3. Implementation: Unified `ZlibEncoder` (gzip/deflate windowBits variant) with streaming context & one-shot paths; optional via build flag.
4. Streaming Integration: Delayed header emission until activation decision ensures accurate `Content-Encoding`.
5. Buffering Strategy: Pre-threshold staging buffer limited by `minBytes`; encoder output chunks forwarded through existing chunked writer.
6. Opt-out: Per-response suppression by providing a `Content-Encoding` header (e.g. `identity`). Automatic compression is skipped when present.
7. Vary header: Automatic `Vary: Accept-Encoding` insertion (config flag) when compression applied.
8. Q-value precedence tests (buffered & streaming) validating negotiation correctness.

Planned extensions:

1. Content-Type allowlist default population & enforcement.
2. Additional codecs: (Delivered: brotli, zstd). Future consideration: dictionary-based zstd, static brotli dictionary selection.
3. Compression ratio & bytes-saved metrics integration into RequestMetrics.
4. Pooling / reuse of zlib streams to reduce init cost.
5. Memory cap adaptive strategy for extremely large responses.
6. Stats: Track compressed vs original bytes (ratio) per request metrics structure.
7. (Delivered) Brotli path integrated; future: static dictionary / custom encoder params.

## Testing Roadmap Highlights

| Feature | Test Concepts |
|---------|---------------|
| Write Buffer | Mock partial writes (small SO_SNDBUF), ensure complete delivery |
| Streaming | Chunk correctness, HEAD suppression, large sequence writes |
| Header Timeout | Byte drip slower than threshold -> close/408 |
| sendfile | File integrity, large file partial cycles |
| Compression | Accept-Encoding negotiation, size reduced |
| Trailers | Map contains trailers, limit enforcement |
| Metrics | Accumulate counts across keep-alive reuse |
| TLS | Handshake + basic GET with self-signed cert |

## TLS Roadmap (Detailed)

Status Legend Recap: ✅ done, 🛠 in progress, ⏳ planned, ❄ deferred

### Phase 1 (Implemented ✅)

- Optional build flag `AERONET_ENABLE_OPENSSL`; core headers free of OpenSSL types.
- Transport abstraction (`ITransport`, `PlainTransport`, `TlsTransport`).
- Basic server-side handshake using OpenSSL with lazy negotiation on first IO.
- Self-signed ephemeral cert integration test (`http_tls_basic`).
- Clean teardown ordering (no ASAN leaks) and graceful SSL_shutdown best-effort.

### Phase 2 (Robustness & Event Loop Integration)

1. Correct EPOLL re-registration on TLS WANT_* events (partially handled; needs audit for write side fairness)
2. TLS partial write & unified buffering (currently plain path stronger; align semantics)
3. Refined backpressure for TLS WANT_WRITE (throttle earlier vs large queue growth)
4. Failure categorization metrics (alert vs timeout vs EOF)

### Phase 3 (Security Hardening)

- Enforce secure defaults (now configurable min/max present; default policy tightening TBD)
- Curate cipher suites: AES-GCM + CHACHA20-POLY1305; remove legacy / CBC by default (current: user-provided)
- Explicit TLS 1.3 cipher suites optional field (future) + validation
- Enforce strong key sizes; reject RSA < 2048.
- Disable compression (defense vs CRIME) explicitly via options (should be off by default).
- Add structured error reporting mapping OpenSSL error codes / alerts -> internal enums.

### Phase 4 (Feature Expansion)

- Client Certificate Authentication (mTLS):
  - Config: CA file/dir, verify depth, optional enforcement flag.
  - Expose verification result to handler (e.g., peer subject) or set 4xx on failure.
- ALPN Support: (DONE) foundation laid for future HTTP/2
- Session Resumption:
  - Enable session tickets; rotating ticket key (time-based) or disable by default.
  - Metrics: resumed vs full handshakes.
- Hot Certificate Reload:
  - Atomic swap of `SSL_CTX` created off-thread; reference counting with shared_ptr.

### Phase 5 (Advanced Capabilities / Nice-to-Have ❄)

- OCSP Stapling (configurable staple file / refresh strategy).
- 0-RTT (TLS 1.3 early data) – only after idempotency safeguards.
- Key logging hook (for debugging) behind explicit insecure flag.
- Curve preference and explicit group selection.
- Session cache statistics & custom callbacks.

### Phase 6 (Observability & Tooling)

- Metrics: (PARTIAL) success, client cert presence, ALPN mismatch, distributions, durations implemented; remaining: failures by category, active TLS counts, resume stats.
- Logging: (PARTIAL) handshake success line implemented; add failure reason path.
- Tracing hooks: optional compile-time enable for per-handshake spans.

### Testing Matrix Additions

| Category | Test Cases |
|----------|------------|
| Negative Handshakes | Invalid cert path, mismatched key, expired cert, unsupported protocol version |
| mTLS | Required client cert missing, invalid CA, successful mutual auth |
| ALPN | Client offers [h2, http/1.1]; server selects http/1.1 |
| Session Resumption | Full then resumed handshake (check `SSL_session_reused`) |
| Backpressure | Force small SO_SNDBUF; verify no busy loop with WANT_WRITE |
| Stress | N concurrent TLS clients, large payload streaming & rapid connect/disconnect |
| Hot Reload | Swap cert mid-run; new connections use new chain, existing unaffected |

### Open Implementation Tasks (Updated Snapshot)

- [ ] WANT_{READ,WRITE} epoll path audit (optimize removal of EPOLLOUT after TLS flush)
- [ ] TLS partial write alignment with plain transport buffering
- [ ] Failure metrics (timeouts vs alerts vs peer close)
- [ ] Cipher suite policy helper (secure default set if user omits list)
- [ ] TLS 1.3 cipher suites explicit config (optional)
- [ ] Session tickets (enable/disable + resumed handshake metrics)
- [ ] SNI multi-cert / context switching
- [ ] Hot cert/key reload (atomic swap)
- [ ] Active TLS connection gauge
- [ ] Handshake p50/p95 latency snapshot (histogram or rolling window)
- [ ] HTTP/2 prototype (leveraging existing ALPN & transport abstraction)
- [ ] Revocation / OCSP stapling support

### Notes

- Keep OpenSSL usage fully isolated inside `tls/` to maintain optional dependency nature.
- Avoid scattering `#ifdef AERONET_ENABLE_OPENSSL` outside TLS boundary except for narrow integration points.
- Future HTTP/2 work will depend on ALPN; design ALPN config with extensibility in mind.

## Open Questions

- Should streaming handler coexist with existing handler (dispatch precedence) or replace it? (Proposed: allow both; pick streaming if a streaming handler is set.)
- Provide synchronous `write()` only, or future-based async? (Initial: synchronous + backpressure boolean.)
- Expose per-chunk flush hints? (Defer until performance tuning.)

## Contribution Notes

- Keep public API additions guarded behind incremental commits with tests.
- Preserve single-thread invariant; multi-thread scaling remains multi-reactor.
- Avoid introducing blocking syscalls in hot path (except unavoidable kernel I/O).

---
This roadmap will evolve as features land. Feel free to edit, annotate progress, or open issues referencing specific rows.
