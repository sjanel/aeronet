# CHANGELOG

All notable changes to aeronet are documented in this file.

## [Unreleased]

### Bug fixes

- Correctly update the `Content-Length` header when using `bodyAppend()` on `HttpResponse` from captured body.
- Correctly format the `HttpResponse` when using **HEAD** method with **trailers** (previously erroneously kept the full payload).
- **HEAD** responses with trailers now correctly omit the body as per RFC 7230 Section 4.3.2, and do not switch to chunked encoding.
- `Accept-Encoding` header parsing is now closer to RFC 7231 Section 5.3.4 when duplicate encodings with different `q` values are present and picks the highest `q` value.

### Breaking Changes

- Minor validation enforcement: `HttpServerConfig::globalHeaders` now MUST be key value separated by `http::HeaderSep`.
- Removed `telemetryContext()` methods from `HttpServer` and `SingleHttpServer`. You can construct a custom `TelemetryContext` instead if needed.
- Telemetry metric methods (including `DogStatsD` ones) are no more `const` qualified (see why in [improvements](#improvements) section).
- Check at runtime if header name and value about to be inserted in a response are valid, otherwise throws `std::invalid_argument`
- HttpResponse constructor with concatenated headers throws `std::invalid_argument` if expected format is not respected.
- `HttpRequest` query parameter API changed: `queryParams()` no longer returns the non-alloc iterable range — it now exposes a map-like view over parsed query parameters where duplicate keys are collapsed (last-value wins). The previous iteration semantics (preserve duplicate order) are available via the new `queryParamsRange()` method. If you used `queryParams()` with **structured bindings** and that there were no **duplicate** keys in your URLs, **no code change is needed**.
- Previously indicated as **undefined behavior**, setting `Content-Type` and `Content-Length` is now prohibited using the `header` and `headerAddLine` methods.
  You should use the dedicated (already existing) `contentType()` and `contentLength()` methods instead for streaming handlers, and set `content-type` along with the body for normal handlers, otherwise `std::invalid_argument` is thrown.

### New Features

- `HttpRequest::makeResponse()` factory methods for simplified response creation with body and content-type.
- `HttpRequest::deferWork()` method to let the main thread come back to the event loop and launch an asynchronous task (in a dedicated thread) to process the request.
- `size` / `length` method helpers in `HttpResponse`, with `reserve` and capacity getters.
- Option `HttpServerConfig::addTrailerHeader` to automatically emit `trailer` header when trailers are added to responses in `HTTP/1.1` only.
- `HttpRequest` now exposes a new map-like for query parameters: `queryParams()` which collapses duplicate keys (last-value wins)
- `HttpRequest` gains the following methods: `hasHeader(key)`, `hasTrailer(key)`, `hasPathParam(key)`, `hasQueryParam(key)`, `pathParamValue(key)`, `pathParamValueOrEmpty(key)`, `queryParamValue(key)`, `queryParamValueOrEmpty(key)`, `queryParamInt(key)`, `headerRemoveLine(key)`, `headerRemoveValue(key, value)`.
- Router paths now accepts asterisk as non terminal segments which are matched as a literal (e.g. `/files/*/metadata`).

### Improvements

- All Header values stored in `HttpResponse` and `HttpResponseWriter` are now **trimmed** of leading/trailing whitespace on set.
- `DogStatsD` is now able to **reconnect automatically** if the UDS socket becomes unavailable. The client is also more efficient.
- `WebSocketConfig.maxMessageSize` is now **strictly respected** when decompressing a `WebSocket` message
- Optimized *prepared* (built from `makeResponse()`) `HttpResponse` to avoid allocating body and trailers memory for **HEAD** requests.
- **Faster case insensitive hash using FNV-1a algorithm for header name lookups**, and optimized version of `tolower` - Use [City hash](https://github.com/google/cityhash/tree/master) elsewhere (for standard strings)
- `HttpRequest::queryParamsRange()` satisfies the **C++20 range** concept.
- Reuse encoders contexts instead of recreating them on each request for better performance.
- Faster `HttpResponse::file()` by optimizing body headers update.
- Smaller memory reallocations when using captured body in `HttpResponse`.
- **`MSG_ZEROCOPY` support** for plain text and kTLS transport TCP connections on Linux (with fallback path). Configurable via `HttpServerConfig::withZerocopyMode()` with modes: `Disabled`, `Opportunistic` (default), `Enabled`.
- **`Router` now uses a more efficient path matching algorithm with a radix tree structure**, **it gains around - 40%** in pattern based routes matching speed. Handlers also consume less memory.
- **Reuse** codec contexts in automatic compression / decompression for better performance.
- `HttpResponse::bodyAppend` now reserves memory exponentially to reduce the number of reallocations when appending large bodies in multiple calls.
- **`HttpResponse::body()` capture overloads now require rvalue references**: `body(std::string&&, ...)` and `body(std::vector<std::byte>&&, ...)`. Passing an lvalue `std::string` (e.g. `resp.body(myString)`) now selects the inline `std::string_view` overload. This should not break existing code and would avoid silent copies when the caller passed an lvalue string / vector to the body capture overloads.

### Other

- Support of ARM64 (aarch64) architecture in CI builds and tests.
- Added [Crow](https://github.com/CrowCpp/Crow) to benchmark comparisons in CI.
- Added `body-codec` scenario to scripted servers benchmarks in CI.
- New compile time option `AERONET_ENABLE_ASYNC_HANDLERS` to enable asynchronous request handlers (enabled by default)
- Fix test compilation when `AERONET_ENABLE_WEBSOCKET` is on but `AERONET_ENABLE_ZLIB` is off.
- In scripted servers benchmarks, `all-except-python` (default) server selection now excludes `python` server by default to avoid skewing results (`all` and `python` are still available).

## [1.0.0] - 2026-01-17

### Release Overview

**aeronet v1.0.0** marks the production-ready release of a modern, high-performance HTTP/WebSocket server library for Linux. The library has undergone extensive development and testing, achieving enterprise-grade stability and performance.
`aeronet` is being used in production in good network conditions.

### Key Milestones

- ✅ **HTTP/1.1 Compliance** - 91.6% feature-complete (87/95 features), fully RFC-compliant
- ✅ **HTTP/2 Support** - RFC 9113 implementation with HPACK, multiplexing, and flow control
- ✅ **Enterprise TLS** - OpenSSL integration with ALPN, mTLS, session tickets, kernel TLS offload
- ✅ **Production Performance** - Benchmarked in HTTP/1.1 against major frameworks (Drogon, Pistache, Go, Rust); consistently top or second in RPS and latency depending on scenario
- ✅ **Comprehensive Testing** - ~94% code coverage at this time, tracked in CI
- ✅ **Minimal Technical Debt** - Only 7 minor TODOs (optimizations, not functional gaps)
- ✅ **Security Hardened** - Comprehensive buffer overflow protection, path traversal mitigation, proper input validation

### Features

#### HTTP/1.1 Core

- [x] Request/response parsing with strict RFC 7230 compliance
- [x] Persistent connections with keep-alive and pipelining
- [x] Chunked Transfer-Encoding with trailer header support (RFC 7230)
- [x] Content-Length body handling with configurable size limits
- [x] Streaming responses with backpressure management
- [x] Full HEAD method semantics
- [x] CONNECT tunneling for HTTP proxying
- [x] Expect header handling with 100-continue support
- [x] TRACE method with configurable security policy
- [x] Request header duplicate handling with proper merge strategies

#### HTTP/2 (RFC 9113)

- [x] HPACK header compression with Huffman encoding
- [x] Stream multiplexing with per-stream and connection-level flow control
- [x] ALPN "h2" negotiation over TLS
- [x] h2c (cleartext prior knowledge) mode
- [x] HTTP/1.1 → HTTP/2 upgrade via Upgrade header
- [x] Unified handler API (same HttpRequest type for both protocols)
- [x] Static file responses with zero-copy sendfile awareness
- [x] CORS and middleware support
- [x] PRIORITY frames (optional, configurable)
- ⚠️ **Note:** HTTP/2 CONNECT tunneling not implemented (use HTTP/1.1 instead)

#### Compression & Content Negotiation

- [x] Accept-Encoding negotiation with q-values and server preference
- [x] gzip, deflate (zlib), zstd, and brotli support (feature-flag gated)
- [x] Inbound request body decompression (symmetric with outbound)
- [x] Per-response compression opt-out
- [x] Automatic Vary header management
- [x] Safe decompression limits with proper 413/415 responses

#### TLS/HTTPS

- [x] File-based and in-memory PEM certificate loading
- [x] SNI-based multi-certificate routing
- [x] Optional/required mTLS with validation
- [x] Strict ALPN enforcement with negotiation
- [x] Min/Max protocol version constraints (TLS 1.2+)
- [x] Handshake timeout for Slowloris mitigation
- [x] TLS session tickets with automatic key rotation
- [x] **Kernel TLS (kTLS) with sendfile** - Linux zero-copy offload (opportunistic/required modes)
- [x] Per-server statistics (no global state)
- [x] Hot reload of certificates and trust store
- [x] Comprehensive metrics (handshake duration, failure reasons, ALPN/cipher distributions)

#### Static File Serving (RFC 7233 / RFC 7232)

- [x] Zero-copy sendfile on plaintext, buffered fallback for TLS
- [x] Single-range request support (RFC 7233)
- [x] Conditional request handling (If-None-Match, If-Match, If-Modified-Since, If-Unmodified-Since, If-Range)
- [x] Strong ETag generation
- [x] Directory listing with optional custom CSS
- [x] HTML escaping and URL encoding for safety
- [x] Path traversal protection (.. segments rejected)
- [x] Configurable default index fallback

#### WebSocket (RFC 6455)

- [x] Upgrade negotiation and handshake
- [x] Bidirectional communication with frame handling
- [x] Per-message compression support
- [x] Proper closure sequences

#### Streaming & Chunked Responses

- [x] Incremental body writing via HttpResponseWriter
- [x] Automatic or explicit Content-Length
- [x] Transfer-Encoding: chunked with proper termination
- [x] Trailing headers support
- [x] Backpressure-aware buffering

#### Performance & Architecture

- [x] Single-thread event loop with epoll
- [x] Horizontal scaling via SO_REUSEPORT
- [x] Multi-instance orchestration (HttpServer wrapper)
- [x] writev scatter-gather for efficient I/O
- [x] Zero-allocation hot paths where possible
- [x] Smart shrink_to_fit for reused connections
- [x] Automated CI benchmarks against major frameworks

#### Cloud Native & Observability

- [x] Kubernetes-style health probes (/livez, /readyz, /startupz)
- [x] OpenTelemetry integration (metrics, tracing)
- [x] Dogstatsd metrics export
- [x] JSON stats endpoint
- [x] Per-server (no global) telemetry state
- [x] Graceful drain lifecycle (beginDrain/stop)

#### Routing & Middleware

- [x] Simple exact-match path routing
- [x] Path pattern parameters (e.g., /users/{id}/posts/{post})
- [x] Wildcard terminal segments (e.g., /files/*)
- [x] Global and per-path request/response middleware
- [x] Mixed-mode dispatch (simultaneous fixed and streaming)
- [x] Configurable trailing slash handling (Strict/Normalize/Redirect)

### Security Enhancements

#### Input Validation & Bounds Checking

- ✅ **Buffer Overflow Protection** - All buffer operations guarded by overflow checks with `std::overflow_error`
- ✅ **Integer Overflow Detection** - SafeCast utility and compile-time assertions prevent wrapping
- ✅ **Header Size Limits** - Configurable `maxHeaderBytes` with 431 response
- ✅ **Body Size Limits** - Configurable `maxBodyBytes` with 413 response
- ✅ **Chunk Size Validation** - Prevents chunk explosion attacks
- ✅ **Outbound Buffer Limits** - `maxOutboundBufferBytes` prevents unbounded memory growth
- ✅ **Multipart Form Safety** - Limits on part count, headers per part, and individual part size

#### Path Traversal Protection

- ✅ **Canonical Path Resolution** - Uses `std::filesystem::weakly_canonical` with fallback to absolute paths
- ✅ **Segment Filtering** - ".." segments explicitly rejected in static file handler
- ✅ **Symlink Status Checks** - Uses `symlink_status()` to detect and prevent symlink attacks
- ✅ **Root Confinement** - All resolved paths guaranteed under configured root directory

#### Protocol Security

- ✅ **Forbidden Trailer Headers** - Authorization, Set-Cookie, Content-* and other unsafe headers rejected
- ✅ **Malformed Input Handling** - Invalid HTTP parsed as 400 Bad Request
- ✅ **Slowloris Mitigation** - Header read timeout (configurable, disabled by default)
- ✅ **Decompression Limits** - Expansion guards and per-stream/connection limits
- ✅ **Content-Encoding Validation** - Unknown encodings return 415 Unsupported Media Type
- ✅ **HTTP/2 Frame Validation** - Frame size checks, stream state validation, flow control enforcement

#### Code Quality & Testing

- ✅ **ASAN/UBSAN in CI** - Address and undefined behavior sanitizers catch memory issues
- ✅ **Comprehensive Testing** - 113 tests covering all critical paths
- ✅ **Code Coverage** - ~94% coverage tracked in CI
- ✅ **clang-tidy Enforcement** - Static analysis prevents common pitfalls
- ✅ **Modern C++23** - RAII-based resource management, no raw pointers in public API

#### No Known Vulnerabilities

- ✓ No buffer overflows detected
- ✓ No integer overflows possible (checked or prevented at compile time)
- ✓ No path traversal vulnerabilities
- ✓ No decompression bombs possible (expansion limits enforced)
- ✓ No unvalidated state transitions
- ✓ No global mutable state (thread-safety by design)

### Known Limitations

The following features are NOT implemented in v1.0.0 (planned for future releases):

- ❌ **HTTP/2 CONNECT Tunneling** - Use HTTP/1.1 for proxy tunneling; full implementation requires per-stream tunnel state tracking
- ❌ **Multi-range Responses** - Single ranges only (RFC 7233 multi-range / `multipart/byteranges`)
- ❌ **Server Push** - Intentionally disabled (rarely used by modern clients)
- ❌ **Pluggable Logging Sinks** - Basic logging functional; advanced hooks TBD
- ❌ **Deterministic Network Fault Tests** - Planned for post-v1.0 robustness improvements
- ❌ **OCSP Stapling** - Passive stapling and revocation checking planned

### Breaking Changes from 0.8.x

**None.** The public API has remained stable throughout development. Code built against 0.8.x should compile and run unchanged with v1.0.0.

Minor refinements include:

- PR #306: HttpResponse constructors simplified (Status+body instead of Status+reason)
- PR #304: New `HttpResponse::bodyStatic()` for static storage (backward compatible)
- PR #302: CORS and middleware support extended to HTTP/2 (new optional features)

### Deprecations

None. All public APIs are stable.

### Build & Compatibility

- **C++ Standard** - C++23 required
- **Compiler** - GCC 13+, Clang 21+ (Windows, macOS are not supported)
- **OS** - Linux (glibc and musl libc supported and tested)
- **Dependencies** - OpenSSL 3+ (optional), spdlog (optional), Conan or vcpkg for dependency management

### Performance

**Benchmarks:** Automated CI benchmarks (wrk-based) show aeronet consistently outperforms competitors:

- ✅ Highest requests/sec in most scenarios
- ✅ Lower average latency than comparators
- ✅ Competitive memory usage
- ✅ Responsive event loop (sub-millisecond latencies typical)

**Metrics:** See [Live Benchmark Dashboard](https://sjanel.github.io/aeronet/benchmarks/) for latest results.

### Installation & Getting Started

Full build and installation instructions: [docs/INSTALL.md](INSTALL.md)

Quick start:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/examples/minimal 8080
```

### Future Roadmap

See [docs/ROADMAP.md](ROADMAP.md) for planned features including:

- HTTP/2 CONNECT tunneling
- HTTP/3 / QUIC support (future transport layer)
- Multi-range responses
- Advanced TLS features (OCSP stapling)
- Deterministic network fault testing
- Performance optimizations (h2load benchmarks, security hardening)

### Security Reporting

If you discover a security vulnerability, please email <dev.sjanel@gmail.com>. Do not open public issues for security concerns.

---

**Download:** [aeronet v1.0.0 Release](https://github.com/sjanel/aeronet/releases/tag/1.0.0)

**License:** Aeronet is licensed under the [LICENSE](../LICENSE) file in the repository.
