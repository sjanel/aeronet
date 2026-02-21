# aeronet Roadmap — Planned / Not Implemented

## Recently completed

- **Direct compression**: Inline body streaming compression at `body()` / `bodyAppend()` time via `DirectCompressionMode`. Eliminates a separate compression pass at finalization for eligible inline bodies.
- `makeResponse` helpers from the handlers to reduce memory moves by adding all global headers at once.
- `MSG_ZEROCOPY` support for large payloads on Linux (with fallback path). Configurable via `HttpServerConfig::withZerocopyMode()` with modes: `Disabled`, `Opportunistic` (default), `Enabled`. Threshold is 16KB. See [zerocopy.hpp](../aeronet/sys/include/aeronet/zerocopy.hpp). Now supports kTLS connections by bypassing OpenSSL's SSL_write and using sendmsg() directly on the kTLS socket.

## High priority

- **HTTP/2 CONNECT tunneling** (RFC 7540 §8.3): Currently returns 405 Method Not Allowed. Full implementation requires per-stream tunnel state tracking, upstream TCP connections, and bidirectional DATA frame forwarding. Users needing CONNECT tunneling should use HTTP/1.1 instead, which has full support.
- **HTTP/2 Performance Optimization & Testing** (h2load benchmarks):
  - Micro-benchmarks for stream multiplexing efficiency
  - h2load-based load testing scenarios (concurrent streams, various payload sizes)
  - Performance regression detection in CI
  - Identify and optimize hot paths in HPACK, flow control, and frame processing
- **Security Hardening Audit**:
  - TLS fingerprinting hardening (avoid leaking version/cipher info in edge cases)
  - Memory scrubbing for sensitive data (handshake keys, session tickets)
  - Formal security review of HTTP/2 frame handling and state machines
  - Fuzzing harness integration (libFuzzer + AFL)

## Medium priority

- **Multipart / multiple-range responses** (`multipart/byteranges`) support (RFC 7233 multi-range)
- **Structured logging / pluggable sinks** - Basic logging functional; advanced hooks allow custom formatters/destinations
- **Enhanced parser diagnostics** (byte offset in parse errors for better debugging)
- **Direct compression option for HEAD**: optional config to allow HEAD responses to match GET headers
  (Content-Encoding + compressed Content-Length) when desired.
- **Performance improvements**:
  - `TCP_CORK` / `TCP_NOPUSH` for response header/body coalescing
  - Further hot-path cache locality optimization
- Enhance `telemetry` with more detailed HTTP/2 metrics: per-stream stats, HPACK compression ratios, frame type distributions.
  - Support tags/labels for metrics

### Performance improvement ideas

- Use kernel helpers where appropriate: `sendfile`, `TCP_CORK` / `TCP_NOPUSH` to coalesce.
- Enforce backpressure correctness to avoid overload and wasted work.
- Focus on cache locality in hot paths; measure before/after.
- Profile and optimize HTTP/2 HPACK decoding (currently identified as optimization candidate).
- `io_uring` support for Linux (future major feature, likely separate transport layer implementation).

## Long-term / Nice-to-have

- **HTTP/3 / QUIC Support** - Likely separate transport layer implementation (future major feature)
- **Fuzz Harness Integration** - libFuzzer targets for HTTP/1.1 and HTTP/2 parsing
- **OCSP Stapling & Advanced TLS** - Passive stapling with cached responses, CRL hooks, key logging (debug only)
- **Per-SNI mTLS Policies** - Different client cert requirements per SNI hostname
- **Advanced Metrics** - Histogram/percentile latency buckets, per-route stats

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

## Realistic Network Testing

Goals

- Improve confidence that aeronet behaves correctly under latency, jitter, packet loss, reordering, partial delivery, and connection resets.
- Catch protocol-level bugs (HTTP/1.1 and HTTP/2), TLS handshake fragility, flow-control and resource-leak issues that do not appear on a perfect local loopback.

Approach and Components

- Deterministic simulated-network unit tests (high priority):
  - Add an injectable transport/socket abstraction used by protocol layers so tests can replace the real socket with a `TestSocket` implementation.
    - `TestSocket` capabilities: partial reads/writes, configurable delays (simulated timers), reordering, duplication, injected resets, and deterministic pseudo-randomness with a seed.
    - Target areas: HTTP/2 frame reassembly and flow-control, HTTP/1.1 chunked transfer edge cases, TLS handshake fragmentation handling, and higher-level timeouts.
    - Tests are deterministic, fast, and run in PRs.

- Proxy-based user-space fault injection (medium priority):
  - Integrate Toxiproxy or a small custom proxy harness for tests that exercise full binaries without requiring NET_ADMIN privileges.
  - Use the proxy to inject latency, connection cuts, truncation and partial writes to validate end-to-end behaviors.

- Kernel-level integration using `tc netem` and network namespaces (lower priority):
  - Create e2e tests that run client & server in separate Linux network namespaces connected by a veth pair with `tc netem` rules applied.
  - Simulate real TCP behaviours (retransmits, delayed ACKs, segment coalescing, zero-window events) that only the kernel stack exhibits.
  - These tests are heavier and run in nightly or scheduled CI only; they are optional for PRs because they require privileged runners and can be flaky.

Test Design & Best Practices

- Start with syscall error injection tests (EINTR, EAGAIN, EPIPE, ECONNRESET) and partial I/O.
- Prefer deterministic `TestSocket` unit tests for core protocol logic: easier to reproduce and debug.
- For integration tests, capture detailed artifacts on failures: pcap, logs, and deterministic seeds used by the test harness.
- Expose test-time configuration hooks (shorter timeouts, deterministic timers) so tests run quickly and reliably.

CI Policy

- PRs: run all deterministic unit tests (including `TestSocket` simulated-network tests).
- Nightly: run proxy-based and `tc netem` integration suites; mark these jobs non-blocking for PRs.

Milestones (suggested)

1. Add `TestSocket` abstraction and 30 deterministic unit tests covering HTTP/2 framing, window-update races, and HTTP/1.1 chunked edge cases.
2. Add simple Toxiproxy-based harness and a handful of end-to-end tests (connection cut, high latency, truncated responses).
3. Add `ip netns` + `tc netem` scripts in `tests/e2e/` and integrate nightly CI job.
4. Collect failure artifacts (pcap + logs) and add tooling to reproduce failing scenarios locally with the same netem parameters.

Acceptance Criteria

- Protocol correctness under simulated faults: no state corruption, proper error propagation, and graceful teardown.
- No resource leaks (sockets, memory) in faulted runs.
- Deterministic unit tests reproduce failures locally with a seed and do not rely on privileged resources.

Notes

- Adding a transport abstraction is a small API design change but yields large testability benefits. Keep the abstraction minimal and efficient in production builds (thin indirection).
- Proxy-based tests are useful when privileged operations are not available in CI.
