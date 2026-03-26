# aeronet Roadmap — Planned / Not Implemented

## High priority

- **HTTP/2 Performance Optimization & Testing** (h2load benchmarks):
  - Micro-benchmarks for stream multiplexing efficiency
  - h2load-based load testing scenarios (concurrent streams, various payload sizes)
  - Performance regression detection in CI
  - Identify and optimize hot paths in HPACK, flow control, and frame processing
- **WebSocket Performance Benchmarks** ✅ — Implemented via `run_ws_benchmarks.py` with k6 scenarios (echo, mix, ping-pong, churn, compression) comparing aeronet, uWebSockets, and Drogon. See `benchmarks/scripted-servers/README.md`.
- **Security Hardening Audit**:
  - TLS fingerprinting hardening (avoid leaking version/cipher info in edge cases)
  - Memory scrubbing for sensitive data (handshake keys, session tickets)
  - Fuzzing harness integration (libFuzzer + AFL)

## Medium priority

- **Windows event loop performance**: The Windows backend uses WSAPoll (readiness‑based, like epoll/kqueue) which is functionally correct but less performant than IOCP for high‑concurrency workloads. A future IOCP backend would require a fundamental architecture shift from readiness to completion semantics.
- **macOS `EVFILT_TIMER` integration**: `TimerFd::armPeriodic()` on macOS is currently a no-op and relies on poll timeouts. Using kqueue's native `EVFILT_TIMER` would improve timer precision but requires event-loop refactoring to accommodate heterogeneous kqueue filter types.
- **Structured logging / pluggable sinks** - Basic logging functional; spdlog backend supports custom sinks/formatters; an aeronet-native sink registration API is not yet exposed
- **Enhanced parser diagnostics** (byte offset in parse errors for better debugging)
- **Direct compression option for HEAD**: optional config to allow HEAD responses to match GET headers
  (Content-Encoding + compressed Content-Length) when desired.
- **Performance improvements**:
  - `TCP_CORK` / `TCP_NOPUSH` for response header/body coalescing
  - Further hot-path cache locality optimization
- Enhance `telemetry` with more detailed HTTP/2 metrics: per-stream stats, HPACK compression ratios, frame type distributions.
  - Support tags/labels for metrics

### Performance improvement ideas

- Use kernel helpers where appropriate: `TCP_CORK` / `TCP_NOPUSH` to coalesce.
- Enforce backpressure correctness to avoid overload and wasted work.
- Focus on cache locality in hot paths; measure before/after.
- Profile and optimize HTTP/2 HPACK decoding (currently identified as optimization candidate).
- `io_uring` support for Linux (future major feature, likely separate transport layer implementation).

## Long-term / Nice-to-have

- **HTTP/3 / QUIC Support** - Likely separate transport layer implementation (future major feature)
- **Fuzz Harness Integration** - libFuzzer targets for HTTP/1.1 and HTTP/2 parsing
- **OCSP Stapling & Advanced TLS** - Passive stapling with cached responses, CRL hooks, key logging (debug only)
- **Per-SNI mTLS Policies** - Different client cert requirements per SNI hostname
- **Advanced Metrics** - ~~Histogram/percentile latency buckets~~ ✔ (implemented via `TelemetryContext::histogram()` + `TelemetryConfig::addHistogramBuckets()`); per-route stats not yet implemented

### TLS enhancements (detailed roadmap)

#### Phase 3 (Advanced / Enterprise)

- OCSP stapling (passive, cached)
- Optional CRL / revocation hooks
- ~~Histogram / percentile metrics~~ ✔
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
