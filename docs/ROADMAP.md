# Aeronet Roadmap & Feature TODOs

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
| ⏳ | Partial Write Buffering & Backpressure | Handle partial socket writes safely | Per-connection output buffer, EPOLLOUT re-arm, max buffer cap | None (current) |
| ⏳ | Streaming / Outgoing Chunked Responses | Send dynamic data without pre-buffering | HttpResponseWriter API, auto chunked if no length | Write buffering |
| ⏳ | Header Read Timeout (Slowloris) | Abort very slow header arrivals | Track header start time + last progress, 408/close | Timing sweep |
| ⏳ | Request Metrics Hook | Expose per-request stats | Struct with method, status, durations, bytes | None |
| ⏳ | Zero-Copy sendfile() Support | Efficient static file responses | Fallback to buffered if partials, track offset | Write buffering |

## Medium Priority (Features & Developer Experience)

| Status | Feature | Summary | Notes |
|--------|---------|---------|-------|
| ⏳ | Trailer Header Parsing (Incoming) | Parse and expose chunked trailers | Size limit + map storage |
| ⏳ | Structured Logging Interface | Pluggable logger levels | Replace stderr printing |
| ⏳ | Compression (gzip/br) | On-the-fly body compression | Threshold + content-type allowlist |
| ⏳ | Graceful Draining Mode | Stop accepting new connections; finish in-flight | server.beginDrain() + state flag |
| ⏳ | Enhanced Parser Diagnostics (offset) | Provide error location info | Extend callback signature |

## Longer Term / Advanced

| Status | Feature | Summary | Notes |
|--------|---------|---------|-------|
| ⏳ | TLS Termination (OpenSSL) | Basic TLS handshake + read/write | Increases complexity significantly |
| ⏳ | Fuzz Harness Integration | libFuzzer target for parser | Build optional, sanitizer flags |
| ⏳ | Outgoing Streaming Compression | Combine streaming + compression | Requires chunked writer |
| ⏳ | Routing / Middleware Layer | Lightweight dispatch helpers | Optional layer atop core |

## Completed Recently

| Feature | Notes |
|---------|-------|
| Parser Error Enum & Callback | `ParserError` with granular reasons + hook |
| RFC7231 Date Tests & Caching Validation | Format stability + boundary refresh tests |
| Request Processing Refactor | Split monolithic loop into cohesive helpers |
| Chunked Decoding Fuzz (Deterministic) | Random chunk framing test with seed |
| HEAD Max Requests Test | Ensures keep-alive request cap applies to HEAD |
| Malformed & Limit Tests | 400 / 431 / 413 / 505 coverage |

## Proposed Ordering (First Pass)

1. Partial Write Buffering
2. Streaming Response API
3. Header Read Timeout
4. Metrics Hook
5. sendfile Support
6. Trailer Parsing
7. Logging Interface
8. Compression
9. Draining Mode
10. Enhanced Parser Diagnostics (offset)
11. TLS
12. Fuzz Harness (extended)

## Design Sketches

### HttpResponseWriter (Streaming)

```cpp
class HttpResponseWriter {
public:
  void setStatus(int code, std::string_view reason = {});
  void addHeader(std::string_view name, std::string_view value);
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
  w.addHeader("Content-Type", "text/plain");
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
- Configurable cap: `withMaxWriteBufferBytes(size_t)`; exceed -> close or 503.

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
