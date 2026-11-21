# aeronet Roadmap - Planned / Not Implemented

## High priority

- **Security Hardening Audit**:
  - Fuzzing harness integration (libFuzzer + AFL), first for the HTTP/1.1 and HTTP/2 parsers

## Medium priority

- **Pluggable logging sink API (non-access logs)** - spdlog backend supports custom sinks/formatters; an aeronet-native sink registration API is not yet exposed.
- **Enhanced parser diagnostics** (byte offset in parse errors for better debugging)
- **Manage TE header in the client**: currently the TE header is reserved in the framework, so it cannot be sent. It could be a good idea to have a flag in the config to allow sending TE headers in the client with trailers essentially (because trailers are natively supported by aeronet). If `TE: trailers` is sent, in HTTP/1.1 we also need to add `Connection: TE`.
- **Thread pool for `deferWork()`**: each call creates an OS thread (see [Limitations](guides/async-handlers.md#limitations)). A bounded pool would reuse them, cap their number and provide backpressure when the work piles up. The same pool could serve the [asynchronous reads of cold files](#asynchronous-reads-of-cold-files).
- **Protocol Buffers, next steps** (after the [delivered](modules/protobuf.md) bodies and `Accept` negotiation):
  - ProtoJSON options (`ignore_unknown_fields`, printing the fields at their default value) and deterministic binary serialization, behind an aeronet options struct (a server-wide `ProtobufConfig`, loadable from configuration files), since their names differ between protobuf versions;
  - streaming: length-delimited message streams over `HttpResponseWriter` and `readBody()`;
  - Connect unary (a plain `POST` with `application/proto`), then gRPC unary over HTTP/2 (5-byte message framing, `grpc-status` trailers, `grpc-encoding`, `grpc-timeout`).

## Low priority

- **Windows event loop performance**: The Windows backend uses WSAPoll (readiness‑based, like epoll/kqueue) which is functionally correct but less performant than IOCP for high‑concurrency workloads. A future IOCP backend would require a fundamental architecture shift from readiness to completion semantics.
- **Linux `io_uring` backend**: its [design study](#linux-io_uring-backend-design-study) measured a small ceiling on the development machine (+3.2% to +3.6% on HTTP/1.1 keep-alive, +7% on connection churn), so its first stage only measures it on GitHub-hosted runners, before any library code.
- **Asynchronous reads of cold files**: a page-cache miss blocks the whole event loop for 80 µs to 0.8 ms. [Design study](#asynchronous-reads-of-cold-files), a thread pool first.
- **Encoded slash handling option**: an encoded slash (`%2F`) in the request path is always answered `400`. Some APIs put one in an identifier (npm scoped packages `/@scope%2fname`, GitLab `/projects/group%2Fproject`), and Tomcat, Apache and Spring Security let it through on demand. A parsing setting in `HttpServerConfig` (not `RouterConfig`: the path is decoded before routing, and `req.path()` is also read by middleware and static files) could add a `Keep` mode leaving `%2F` encoded in `req.path()`, so that `/{id}` captures `group%2Fproject`. It must then also keep `%25` encoded, otherwise `/a%252Fb` and `/a%2Fb` give the same path. A `Decode` mode (the former behavior) would only help catch-all and static routes.
- **macOS `EVFILT_TIMER` integration**: `TimerFd::armPeriodic()` on macOS is currently a no-op and relies on poll timeouts. Using kqueue's native `EVFILT_TIMER` would improve timer precision but requires event-loop refactoring to accommodate heterogeneous kqueue filter types.

### Performance optimization backlog

Status: repository-wide source audit completed on 2026-07-22; the opportunities below are not yet implemented or
proven faster. They are ordered by expected leverage, not by unverified percentage estimates.

#### P2 - cache locality, dispatch and repeated scans

- **HTTP/2 stream lookup and hot/cold layout** - extend the stream benchmark beyond insert/erase to randomized lookup, DATA/WINDOW_UPDATE processing and close/prune churn at 1, 100, 1,000 and 10,000 active streams. Use the profile to decide between the current flat hash map, a tiny recent-stream cache, denser indexing, or splitting frequently touched state from callbacks, header storage and other cold fields.
- **HPACK dynamic table insertion stall** - when the responses carry indexed fields whose value changes each time (`x-request-id`, `traceparent`, or the scripted `/headers?count=10&size=32` route), `HpackDynamicTable::add()` takes 1.3% of the server's retired ops (IBS) but 9% of its cycles (measured 2026-11-16, Zen 5, GCC Release, h2c). Nearly all its samples fall on the instruction right after the call to `http::Header(Header&&, name, value)`, which reuses the buffer of the evicted entry: a load of the new header's pointer from the temporary `Header` the constructor just wrote on the stack. All entries have the same size in that scenario, so the constructor does not realloc: the stall is not allocator work, and store-forwarding failures (`ls_bad_status2.stli_other`) explain only 5% of it. To check, in order: (1) a store-forwarding or memory-ordering stall on the stack temporary - build the new entry in place in `_entries` instead of through a temporary moved into it; (2) cycles-sampling skid from the name/value copies or from the return - IBS tag-to-retire latency per instruction; (3) the compaction of `_entries` in `evict()`, a memmove of the live entries once the retired prefix reaches their count. Judge each change with `perf stat` (cycles and instructions per iteration, deterministic) on `BM_HpackEncodeResponsesOverflowingDynamicTable` (1 and 10 fields), then on the in-process `aeronet-bench-internal-http2-request` (fields that repeat must not regress), then end to end on `/headers`. A related, already measured idea: keeping the capacity of evicted buffers (a capacity trailer) avoids the realloc of 17.5% of the insertions of the hpack-test-case corpus, about 1.5 ns per insertion.
- **kTLS send for file responses only** - kTLS is off by default since 2026-11-18: on three GitHub runner CPUs and a Zen 5 workstation, kTLS send serves in-memory bodies of 16 KiB to 1 MiB 5 to 54% slower than user-space TLS (1.84M kernel cycles per MiB at 1 MiB on Zen 5, against 0.90M kernel and 0.62M user ones), while files sent with `sendfile()` gain up to 91% (see [Kernel TLS](protocols/tls.md#kernel-tls-ktls)). Enabling it with the first file response of a connection would keep the gain without the loss, but OpenSSL decides kTLS at the handshake (`SSL_OP_ENABLE_KTLS`) and does not expose the traffic keys and record sequence numbers that enabling it later needs, and the in-memory responses that follow on the connection would then pay for it.
- **HTTP header mutation/search** - `HttpMessage` repeatedly scans the flat CRLF buffer for lookup, append, remove and override operations. First profile realistic 4/8/16/32-header construction and middleware mutation. If scans dominate, evaluate known-header offsets or a lazy compact index that is invalidated on mutation without adding allocation or meaningful `sizeof(HttpMessage)` cost to the common case.
- **TCP cork syscall policy** - Linux response dispatch wraps a flush in `TcpCorkGuard` only when it takes several writes (a response head then its file, output queued around a file, user-space TLS output longer than one record). Measure the packets and syscalls of these cases against no cork - a small file after its head costs two `setsockopt` calls to save one partial segment - and keep cork only where packet reduction outweighs the extra syscalls and latency.
- **JWT/JWKS verification primitives** - `Jwks::find()` scans every key for each token and base64url decoding classifies every character through branches. Add end-to-end JWT decode/verify benchmarks by algorithm, token size and JWKS size, then compare linear lookup with a compact sorted/hash index and branch classification with a 256-byte decode table. Include missing-`kid` and malformed-input cases so failure paths do not regress.
- **Hot object layout audit** - record `sizeof`, alignment and cache-line access profiles for `ConnectionState`, `Http2Stream`, `WebSocketHandler`, `HttpMessage`, `HttpRequestView` and client connection state at 10 K simulated connections/streams. Move genuinely cold optional state behind existing natural-empty/pool mechanisms only when cache results beat the added indirection. `ConnectionState` lifecycle flags are already bit-packed; do not reopen that item without new layout evidence.
- **Compression/decompression retained memory** - benchmark codec context reset, scratch-buffer growth and peak retained capacity across alternating tiny/large bodies and many keep-alive sessions. Tune growth/shrink thresholds per codec, and continue the Brotli reset/reuse research, only when allocation traces show a benefit over the existing reusable `BufferCache` / `ObjectArrayPool` paths.

#### P3 - platform and build experiments

- **Configurable/adaptive HTTP client reads** - the client currently requests fixed 16 KiB chunks for HTTP/1.1 and HTTP/2 receives. Benchmark configurable 4/16/64 KiB and adaptive growth under small responses, bulk transfers, TLS and constrained-memory workloads before exposing a knob or policy.
- **Repeat-connection latency** - add a bounded TTL DNS cache and client-side TLS session reuse only after benchmarks separate resolver, TCP and handshake time. Expiry, address rotation and failed-resumption fallback must remain explicit.
- **Profile-guided builds** - evaluate PGO (and BOLT on supported Linux toolchains) in the benchmark binaries against the existing Release + IPO baseline. Keep this an opt-in build/documentation path: installed library code must not assume the benchmark host's CPU or workload.
- **Platform event backends** - continue the dedicated IOCP and native macOS `EVFILT_TIMER` work listed in [Low priority](#low-priority), and the Linux `io_uring` one of its [design study](#linux-io_uring-backend-design-study). Treat these as transport/backend projects with end-to-end results, not local syscall substitutions.

#### Benchmark ideas

Existing `hpack_bench`, `http2_flow_control_bench` and WebSocket mask benchmarks cover basic steady-state operations, so
extend them instead of duplicating them.

| Benchmark gap | Decision it must support |
| --- | --- |
| Stream randomized lookup, frame processing and prune churn through 10 K streams | Stream map/cache and hot/cold split |
| Response-size syscall counts, plain + TLS | TCP cork threshold |
| Header lookup/mutation with 4 through 32 fields | Flat scan versus compact/lazy indexing |
| JWT decode/verify with 1 through 1,000 JWKs | `kid` index and base64url decode strategy |
| Codec alternating-size sessions with allocation and peak-capacity counters | Scratch growth/shrink and context reuse |
| Fragmented + compressed WebSocket message processing | End-to-end frame/reassembly path beyond mask-only throughput |
| Hot-structure `sizeof` and 10 K-object random-access profile | Layout changes without speculative padding claims |

### Linux `io_uring` backend - design study

Status: design study of 2026-11-18, against `main` at `1242e218`, deciding whether and how aeronet should get an `io_uring` backend on Linux; nothing is implemented.

#### Recommendation

**Do it in stages, and expect to stop after the first one.** Only the hybrid seam is worth building: the ring becomes the blocking wait of the event loop and the input path of plain connections, while output, TLS, files, timers and `HttpClient` keep the readiness code, and epoll stays the default and the fallback. Its measured ceiling on the development machine is +3.2% to +3.6% on HTTP/1.1 keep-alive and +7% on connection churn, for an estimated 1,500 to 2,000 lines, a second Linux input path to keep correct across kernels, and buffer-lifetime bugs that AddressSanitizer does not see (verified below). That is below the [stop criterion](#staged-plan-and-stop-criterion), so stage 0 is a measurement, not a backend: run the same probe on GitHub-hosted runners, and start stage 1 only if the ceiling there reaches **+8% requests/s on HTTP/1.1 plain keep-alive at 100 connections, or +12% connections/s on churn**. Whatever the outcome: take the cheaper epoll-side wins first (no `epoll_ctl(DEL)` before `close()`, one read per timer tick), so that the ring is compared with the best epoll loop; and keep io_uring off by default at both levels (CMake option and runtime field), falling back to epoll with one log line when the kernel, a sysctl or a seccomp profile refuses it. Asynchronous reads of cold files are a separate question with a stronger case, since a page-cache miss blocks the whole loop for 80 µs to 0.8 ms here; [their own study](#asynchronous-reads-of-cold-files) finds that a thread pool may serve them as well as a ring.

#### Baseline and evidence

Network cost study of 2026-11-16/17 (commit `d1dd9f9d`, one event-loop thread, `GET /ping` on loopback, syscalls counted at the libc boundary by an `LD_PRELOAD` shim):

| Scenario | Requests/s | Syscalls per request |
| --- | ---: | --- |
| HTTP/1.1 plain, 100 connections | 200.9k | 2.01: `recv`, `send`, 0.011 `epoll_wait`, no `epoll_ctl` |
| HTTP/1.1 plain, 10k connections | 135.8k | 2.00 |
| HTTP/1.1 plain churn (`Connection: close`, 100), per connection | 78.9k conn/s | 7.02: `accept4`, `epoll_ctl` ADD, `recv`, `send`, `ioctl(SIOCINQ)`, `epoll_ctl` DEL, `close` |
| h2c, 100 connections x 10 streams | 1,038k | 0.20: 0.1 `recv`, 0.1 `send` |
| HTTP/1.1 user-space TLS, 100 connections | 154.4k | 3.01, 2 since #968 removed an `EAGAIN` read |

Throughput probe of 2026-11-10 (a minimal ping server, not aeronet; one pinned thread, `wrk -t4`, 5 alternating 3 s rounds, ring flags `SINGLE_ISSUER | DEFER_TASKRUN`, 1024 provided buffers of 4 KiB), against epoll + `recv` + `send`:

| Variant | 64 connections | 256 connections |
| --- | ---: | ---: |
| Ring `recv` (single-shot, provided buffers), `send()` syscall | +3.2% | +3.6% |
| Ring `recv` (multishot), `send()` syscall | +0.4% | +1.5% |
| Ring `recv` + ring `send` | -4.0% | -2.8% |

Churn with multishot accept (no peer address) + ring `recv`: +7% (85k to 91k connections/s). At 8 connections, ring `recv` was about 3% slower (one run).

Plain keep-alive already makes one `recv` and one `send` per request: a ring removes the `recv` syscall and the per-event epoll work, about 300 TSC ticks (0.15 µs) of a 5 µs request. Ring sends lose: a syscall costs 89 ticks and a batched ring operation 51 to 59, but a send + receive pair costs as much through the ring (747 ticks, batch of 16) as with two syscalls (740 to 780), and its completion must then be processed.

Both studies ran on one laptop (Zen 5, kernel 7.0.0-43) with a security sensor and a BPF LSM, where a keep-alive request costs about 23k kernel cycles. Two effects pull in opposite directions there: each syscall carries extra hooks (every removed syscall is worth more) and kernel cycles dominate each request (percentages are diluted). Only CI-class hardware settles the net effect: that is stage 0.

**Markers**: **[code]** read in the sources at `1242e218`; **[netstudy]** measured by the network cost study above (a local report, not in the repository); **[probe]** observed on kernel 7.0.0-43 by the 2026-11-10 study or by the probes of 2026-11-18 ([Probes](#probes)); **[doc]** kernel or liburing documentation, not checked here, `(?)` marking the least certain versions; **[assumed]** reasoning only.

##### What depends on readiness today [code]

| File | Functions | Readiness assumption | In the hybrid |
| --- | --- | --- | --- |
| [event-loop.cpp](../aeronet/sys/src/event-loop.cpp) | `EventLoop::add`, `mod`, `del`, `poll` | `epoll_ctl` / `epoll_wait`, `EventFd` laid out as `epoll_event`, edge-triggered | Unchanged, nested in the ring |
| [timer-fd.cpp](../aeronet/sys/src/timer-fd.cpp), [event-fd.cpp](../aeronet/sys/src/event-fd.cpp) | `TimerFd::drain`, `EventFd::read` / `send` | Maintenance tick and cross-thread wakeup are readable fds | Unchanged, in epoll |
| [transport.hpp](../aeronet/sys/include/aeronet/transport.hpp) | `PlainTransport::read`, `write`, `Transport::hasPendingReadData` | `ReadReady` / `WriteReady` mean "wait for an event"; data buffered above the socket is polled explicitly | Ring connections read the completion's bytes first |
| [zerocopy.cpp](../aeronet/sys/src/zerocopy.cpp) | `ZeroCopyState::pollCompletions` | Error queue read when `EPOLLERR` fires | In epoll while sends are in flight |
| [sendfile.cpp](../aeronet/sys/src/sendfile.cpp), [tcp-connector.cpp](../aeronet/sys/src/tcp-connector.cpp) | `Sendfile`, `ConnectTCP`, `WaitForConnectCompletion` | `EAGAIN` waits for writable; connect completes with `POLLOUT` + `SO_ERROR` | Unchanged |
| [tls-transport.cpp](../aeronet/tls/src/tls-transport.cpp) | `TlsTransport::read`, `hasPendingReadData`, `trackShortSocketReads`, `handshake` | Socket BIO read until drained; records read ahead raise no edge; `WANT_READ` / `WANT_WRITE` become readiness | TLS stays on epoll |
| [single-http-server.cpp](../aeronet/server/src/single-http-server.cpp) | `eventLoop`, `handleConnectionEvent` | Blocks in `poll()` (`poll(false)` while inputs are deferred), dispatch by fd, an unknown fd is a stale event | Blocks in the ring; recv completions get their own dispatch |
| | `acceptNewConnections`, `handleReadableClient`, `resumeDeferredInputs`, `handleWritableClient` | `add(EventIn \| EventRdHup \| EventEt)` at accept; a short read drained the socket; the fairness budget defers the rest | Plain connections attach to the ring; "drained" arms a recv |
| [connection-io.cpp](../aeronet/server/src/connection-io.cpp) | `enableWritableInterest`, `disableWritableInterest`, `flushOutbound`, `flushFilePayload` | `EPOLL_CTL_MOD` toggles `EventOut`; writes until `EAGAIN`, retried once after enabling the interest (edge race) | Ring connections: `ADD` / `DEL` with `EventOut` only |
| | `deferInput`, `resumeInputIfOutputDrained`, `startLingeringClose`, `discardLingeringInput` | Input left unread raises no new edge; a lingering close reads until EOF | Unchanged: no recv is armed meanwhile; ring recv until EOF |
| | `releaseConnection` | `epoll_ctl(DEL)` then `close()`, which purges pending events | Cancel the armed recv, bump the generation |
| [connection-storage.hpp](../aeronet/server/include/aeronet/internal/connection-storage.hpp) | `ConnectionStorage::iterator(fd)` | Indexed by fd: on Linux "the poller never returns a closed fd" | Generation check first |
| [http-server-lifecycle.cpp](../aeronet/server/src/http-server-lifecycle.cpp) | `initListener`, `runUntilStarted` | `EventLoop` built on the controller thread for `MultiHttpServer` workers, polled on the loop thread | Ring created and closed on the loop thread |
| [tunnel-manager.cpp](../aeronet/server/src/tunnel-manager.cpp) | `registerUpstream`, `handleWritable`, `relayInput`, `readInput` | Connect completion by `EPOLLOUT`; stop reading by `MOD` to `EventOut` only | Tunnel endpoints move to epoll |
| [connection-maintenance.cpp](../aeronet/server/src/connection-maintenance.cpp) | `sweepIdleConnections` | Retries writes whose writable edge was missed | Unchanged |
| [http-client.cpp](../aeronet/client/src/http-client.cpp) | `HttpClient::armLoop`, `waitEvents` | Private `EventLoop`, one fd at a time | Unchanged |

#### Seams compared

| Seam | Expected gain, and why | Size | Risk | Portability |
| --- | --- | --- | --- | --- |
| **(a)** `EventLoop` shim: `add` / `mod` / `del` = multishot `POLL_ADD`, update, remove; `poll()` = `io_uring_enter` | Keep-alive about 0: no `epoll_ctl` to batch in steady state, and the wait is one syscall either way. Churn 2.5% at most (`ADD` + `DEL`: 317 ns of 12.7 µs), most of which skipping `DEL` gives on epoll (+0.6% to +1.1% measured) | Small, `aeronet/sys` only: about 600 lines | Medium: multishot poll is not epoll (a completion per wakeup, ended without `IORING_CQE_F_MORE` on overflow, no `EPOLLEXCLUSIVE`), and a poll request holds a file reference, so `close()` no longer purges events: the generations of [Buffer ownership and lifetime](#buffer-ownership-and-lifetime) are needed anyway. The old `feature/io_uring` branch found this emulation the fragile part | None |
| **(b)** Completion-based transport: multishot recv into provided buffer rings, ring sends, registered files, multishot accept | Below (c) on aeronet's request/response pattern: multishot recv +0.4% to +1.5%, ring sends -2.8% to -4.0%. May do better where syscalls cost more (VMs with mitigations): stage 0 measures it | Very large: asynchronous output (output queue, streaming writers, HTTP/2 `onOutputWritten`, OpenSSL through memory BIOs, `sendfile` through pipes, zerocopy), connections outliving their close. The old branch, a full proactor, added 2,480 lines over 42 files | High: send ordering, buffers held until completion, multishot recv ignoring backpressure | High: two output models in the protocol layers, unless every backend becomes completion-based (the IOCP item of [Low priority](#low-priority)) |
| **(c)** Hybrid: the ring is the wait and the input of plain connections (single-shot recv with buffer selection, armed once the socket is drained), optionally multishot accept; synchronous output; epoll nested in the ring | Ceiling +3.2% to +3.6% keep-alive, +7% churn, about -3% at 8 connections; about 0 for multiplexed h2c and for TLS | Medium: 1,500 to 2,000 lines with tests (ring wrapper 300, buffer pool 150, slots and dispatch 350, about 8 guarded server touch points 250, tests 700); protocol layers and `ConnectionState` unchanged | Medium: armed recvs outlive `close()`, stale completions (verified), close timing | None: Linux only, behind the option |

**Choice: (c).** (a) brings nothing for its risk; (b) loses on the measured pattern and makes the protocol layers depend on the backend. In (c) the ring holds one multishot `POLL_ADD` on the epoll fd [probe: one completion per new readiness, none while idle]; when it fires, `EventLoop::poll(false)` collects the readiness events as today: wakeup eventfd, timerfd, writable waits after `EAGAIN`, zerocopy error queues, TLS connections, tunnels. These cold fds cost one extra syscall only in the iterations where one fires. The ring code is a dedicated server component (`internal::RingInput`, bound in `ServerContext::bindComponents()`): `SingleHttpServer` and `ConnectionIo` only gain guarded calls.

#### Buffer ownership and lifetime

**Inbound buffers.** Today [code], `ConnectionState::transportRead()` reads at the end of `inBuffer`, which for a connection without pending input is the buffer lent by the event loop (`ConnectionIo::lendInBuffer()`, handed back by `releaseServedInBuffer()`): an idle connection holds no input buffer, and request views point into `inBuffer`, so pending input never moves. A recv armed with a buffer of its own would pin it while idle. Provided buffers keep the invariant, the kernel taking one only when data arrives [probe, Q2].

- One buffer ring per event loop: buffers of `minReadChunkBytes` (4 KiB) from one slab allocated with the ring and freed after it.
- A completion's bytes are copied into `inBuffer` (lent or owned) through `transportRead()`, and its buffer returns to the ring before the next wait. Request views never point into a provided buffer, so the input buffer invariants hold as they are. The copy is 45 bytes for `GET /ping`, 4 KiB at most.
- A full buffer, or `IORING_CQE_F_SOCK_NONEMPTY`: the read loop continues with `recv()` into `inBuffer`, as today, so bulk uploads copy only their first 4 KiB once more.
- `-ENOBUFS` (ring empty): a direct `recv()` for that connection, counted.
- Pool size: about 94 events arrive per wait at 100 connections, 9.2k to 10k at 10k connections [netstudy]. Without incremental consumption (`IOU_PBUF_RING_INC`, 6.12 (?)), serving the latter from the pool would take 40 MiB per loop. Proposed: 256 buffers (1 MiB per loop) and `-ENOBUFS` beyond (open question 3).

**What must stay valid while an operation is in flight.**

| Operation | The kernel holds | Until |
| --- | --- | --- |
| `RECV` with `IOSQE_BUFFER_SELECT` | A reference to the socket file; a provided buffer once data arrives; no aeronet memory | Its completion, or its cancellation's |
| Multishot `ACCEPT` (stage 2) | A reference to the listener | Its cancellation: closing the fd does not stop it [assumed, like the verified recv case] |
| Multishot `POLL_ADD` on the epoll fd | A reference to the epoll fd | Ring teardown |
| Buffer ring and slab | Registered memory | Unregistration, after every recv completed |
| An SQE | An fd number, resolved to a file at submission [doc] | Submission |

The last row is a trap: an SQE written during dispatch for a connection that then closes, its fd number reused by a connection accepted in the same iteration, would arm a recv on the new socket with the old token, and the new connection's first bytes would be dropped as stale. So recvs are armed only in the ring wait, right before the submission, from a list checked against the slot generations: dispatch never writes an SQE. `ConnectionState` memory is never handed to the kernel, and a state is recycled as soon as its connection closes.

**`user_data` and generations.** [probe, Q1] With a recv armed on fd 6, `close(6)`, and a new connection accepted as fd 6, the old recv completed naming fd 6 with the old peer's bytes: the fd-indexed storage would hand them to the new connection.

- `user_data` = operation (4 bits) | generation (28 bits) | fd (32 bits).
- A slot of 8 bytes per fd, in a vector indexed like `ConnectionStorage` (`fd - 1`): generation and state bits (recv armed, in the arm list, cancellation queued, in epoll). `ConnectionState` (720 bytes) does not change.
- The generation is bumped at attach and at close. A completion whose generation or operation does not match its slot is stale: its buffer (`IORING_CQE_F_BUFFER`) returns to the ring, its data is dropped, a counter counts it. Aliasing would need 2^28 reuses of one fd number before the next wait.
- Detaching a live connection (a new tunnel endpoint) keeps the generation: a recv completed before its cancellation delivers its bytes to the same connection (appended to `inBuffer`, then `deferInput()`), since they belong to its stream - the first tunnel bytes after `CONNECT`.

**Arming, cancellation and close ordering.**

- **Arm** when a read leaves the connection drained (short read without `SOCK_NONEMPTY`, the condition `handleReadableClient()` computes today) and nothing stops its reading (input blocked by output, `readPaused` during an async handler, a close requested): a `RECV` with `IOSQE_BUFFER_SELECT` and `IORING_RECVSEND_POLL_FIRST`. One recv at most per connection: backpressure is not re-arming, and the kernel never reads ahead of the protocol layer.
- **Close** (`releaseConnection()`): bump the generation, queue an `ASYNC_CANCEL` by `user_data` (not by fd, whose number may be reused in the same iteration) with `IOSQE_CQE_SKIP_SUCCESS`, `close()` as today. [probe, 2026-11-10] the socket lives until the cancellation runs, and the peer sees no FIN before: queued, it runs at the next wait, microseconds later under load. A close that must act at once (abortive close with zerocopy sends in flight, end of a run) submits it right away: one `io_uring_enter`, the price of today's `epoll_ctl(DEL)`, or `IORING_REGISTER_SYNC_CANCEL` (6.0). Open question 4.
- **Lingering close**: `shutdown(SHUT_WR)` sends the FIN at once; reading until EOF works through the ring.
- **Listener close** (stage 2): cancel the multishot accept first.
- **Teardown**, on the loop thread: close the connections (each queues its cancellation), `ASYNC_CANCEL_ANY`, reap until nothing is in flight, unregister the buffer ring, close the ring, free the slab.

#### TLS

Today [code], `TlsTransport` binds its `SSL` object to the socket (`SSL_set_fd()` in `TlsAcceptor`) with read-ahead. `read()` decrypts until the socket is drained, detected by a read-BIO callback (`trackShortSocketReads()`); `hasPendingReadData()` reports records read ahead; writes go through `SSL_write_ex()` record by record, or straight to the socket once kTLS send is on (now off by default).

| Option | What it does to `TlsTransport` | Gain | Verdict |
| --- | --- | --- | --- |
| T1: TLS stays on epoll | Nothing | None | Stages 1 and 2 |
| T2: memory BIO (or a custom `BIO_METHOD`) between ring and OpenSSL | `SSL_set_bio()` instead of `SSL_set_fd()`; handshake flights and alerts flushed from the write BIO explicitly; `WANT_READ` means "no ciphertext buffered"; `trackShortSocketReads()` replaced by the completion flags; no kTLS (OpenSSL enables it through its socket BIO); `releaseBuffers()` covers the BIO buffers; one more copy of the ciphertext, avoided by a custom BIO reading the provided buffer | The read syscall, 1 of 2 per request since #968: about 0.15 µs of 6.5 µs, 2% to 3%. The larger write-side gain (one `send()` for several records, against 65 writes per MiB, about 8% at 1 MiB) needs no ring: a write memory BIO on epoll gives it | Stage 3, once the write memory BIO is measured on epoll, so the ring is not credited with its gain |
| T3: kTLS send and receive, ring operations as on a plain socket | kTLS receive needed (OpenSSL 3.0: TLS 1.2 only, 3.2+: TLS 1.3); a non-data record (alert, KeyUpdate) fails a plain recv with `EIO` [doc], hence `RECVMSG` with a control buffer | Read and write syscalls | Rejected: kTLS send measured 5% to 54% slower than user-space TLS for in-memory bodies on the 3 CI CPU types |

#### Timers

Today [code], one periodic timerfd per loop, at the smallest of the enabled timeouts (keep-alive, header read, body read, TLS handshake) and `pollInterval` (`updateMaintenanceTimer()`), drives the maintenance: the keep-alive deadline heap (`KeepAliveDeadlineQueue`, one O(log n) update per event), header read, body read, request and handshake timeouts as timestamps swept only while some connection needs it, HTTP/2 stream deadlines. The poll timeout never fires on Linux, the timerfd coming first [netstudy].

- **Linked timeouts** (`LINK_TIMEOUT`, 5.5) per recv: rejected. Per request they add an SQE, a completion and an hrtimer armed and cancelled, where the heap costs one update per event; and they bound one operation, while aeronet's timeouts span several: a head sent a byte per second completes a recv each second, so a per-recv timeout never fires (the slowloris case `headerReadTimeout` exists for); keep-alive idleness is also refreshed by output progress (`lastActivity` in `flushOutbound()`); HTTP/2 and WebSocket timeouts have no pending recv to link to.
- **Timeout operations**: the wait's own timeout (`IORING_ENTER_EXT_ARG`, 5.11) replaces `epoll_wait`'s. A multishot `TIMEOUT` (6.4 (?)) could replace the timerfd and its 2 reads per tick: not worth a change, the timerfd stays in the nested epoll.
- **Decision**: the deadline queue, the header read timer and the maintenance tick do not change and do not know the backend.

#### Files and zero copy

Today [code], files up to `inlineFileThresholdBytes` (128 KiB by default) are inlined into the response; larger ones follow the head with `sendfile()` in 2 MiB chunks (`ConnectionState::transportFile()`), with writable interest and one immediate retry on `EAGAIN`; user-space TLS reads 128 KiB chunks with `pread()`. Large in-memory plain responses use `MSG_ZEROCOPY`: buffers retained until completion (`ZerocopyBuffers`), completions read from the error queue on `EPOLLERR`, drain closes waiting for them, otherwise an abortive close and a one second grace period.

- **sendfile and splice**: io_uring has no sendfile operation. Its equivalent, two `SPLICE` operations (5.7) per chunk through a pipe, is bounded by the pipe capacity (64 KiB by default), so more operations per MiB than one `sendfile()` of 2 MiB, which uses the same splice machinery in the kernel [doc]. `sendfile()` stays.
- **The real file-side gain** would be not blocking the loop on page-cache misses, which `pread()` and `sendfile()` of a cold file do today [code]. That is a separate track, independent of the network backend: [Asynchronous reads of cold files](#asynchronous-reads-of-cold-files).
- **`SEND_ZC` (6.0) against the zerocopy module**: the send result comes as a completion, then a notification (`IORING_CQE_F_NOTIF`) once the buffer is released: no error queue, no `SO_ZEROCOPY`, and with registered buffers no page pinning per send [doc]. It fits the existing retention machinery, but its result is asynchronous: zerocopy sends would become the first asynchronous writes, or be submitted and reaped in one `io_uring_enter`. Gains need a NIC: on loopback `MSG_ZEROCOPY` measured -28% at 256 KiB and -37% at 1 MiB [netstudy]. Stage 3, between two hosts.
- **Zerocopy in the hybrid**: a ring connection with zerocopy sends in flight joins the nested epoll for `EPOLLERR` until they complete, like a writable wait.

#### HTTP/2

Today [code], `processSpecialProtocolHandler()` processes every complete frame of the input buffer, one gathered write sends the pending fragments (`getPendingOutputFragments()`), and `onOutputWritten(written)` releases what was written, at once.

- **Many small frames** arrive in one completion as in one `recv()` today, and leave in one gathered write per flush. Per-frame submissions (an SQE and a completion each) are never made.
- **Gain**: multiplexed h2c makes 0.1 recv per request [netstudy], so the ring saves 0.1 syscall entry per request, a few nanoseconds. One stream per connection behaves like HTTP/1.1.
- **Flow control**: HTTP/2 flow control is in WINDOW_UPDATE frames, and the server's backpressure (`pendingOutputSize()` against `maxOutboundBufferBytes`, `stopReadingIfOutputBlocked()`, `relayPausedByBacklog`) works by not reading. A single-shot recv keeps that exact; a multishot recv would keep filling buffers: rejected.
- **Ordering**: synchronous output keeps frames in queue order, partial writes known at once, and the `onOutputWritten()` contract. Ring sends would need at most one send in flight per connection (two sends on a socket armed for poll may complete out of order unless linked [doc]), RST_STREAM, GOAWAY and WINDOW_UPDATE queued behind it, and the DATA buffers of a reset stream kept while an SQE points to them: an ownership change inside the HTTP/2 layer, which the hybrid avoids.

#### Threads

- **One ring per event-loop thread**: per `SingleHttpServer`, so one per `MultiHttpServer` worker, each with its own `SO_REUSEPORT` listener [code]. Nothing is shared (no `MSG_RING`, no `ATTACH_WQ`); connections keep the kernel's hash distribution, imbalanced at 2 connections per thread or fewer [netstudy].
- **Created on the loop thread**: `SINGLE_ISSUER` (6.0) ties the ring to its creating thread (or enabling thread, with `R_DISABLED`) [doc]. `MultiHttpServer` builds its workers' event loops on the controller thread (`ensureNextServersBuilt()`, `initListener()`, then copies) [code], so the ring is created at the start of `runUntilStarted()` and closed at its end, once per run: moves and restarts, done while stopped, never see a live ring.
- **`DEFER_TASKRUN`** (6.1): completions are processed only in `io_uring_enter` with `GETEVENTS` [doc], so task work never interrupts a handler and completions batch at the wait. They are not visible by peeking at the CQ, so `poll(false)` (taken while inputs are deferred) costs one `io_uring_enter`, like today's `epoll_wait(0)` (46 ns).
- **Cross-thread wakeups** (`wakeupFd.send()` for updates, `stop()`, async resumes) keep the eventfd in the nested epoll, read with libc `read()`: no new synchronization, and ThreadSanitizer keeps the edge it derives from that read.
- **SQPOLL: not wanted.** A kernel thread per ring spins on the submission queue, then sleeps after `sq_thread_idle` and needs a syscall to wake: one more busy core per loop, where aeronet maps loops 1:1 to cores, to save a syscall the hybrid already amortizes over a batch. It cannot be combined with `DEFER_TASKRUN` [doc], its CPU is not accounted to the loop thread (pinning and quotas get harder), and some policies restrict it (SELinux `io_uring { sqpoll }`).
- **io-wq**: recv with `POLL_FIRST` and accept are poll-driven, never handed to io-wq [doc]; cap the workers anyway (`IORING_REGISTER_IOWQ_MAX_WORKERS`, 5.15).

#### Detection, fallback and build

**Minimum kernel 6.1** (`DEFER_TASKRUN`), which brings everything else used: provided buffer rings, multishot accept, `POLL_FIRST`, `SOCK_NONEMPTY` (5.19), `SINGLE_ISSUER` (6.0). Ubuntu 22.04's 5.15 and RHEL 9's 5.14 fall back to epoll.

At the start of each run, on the loop thread, when the runtime field asks for the ring:

1. `io_uring_setup()` with `SINGLE_ISSUER | DEFER_TASKRUN | SUBMIT_ALL | CQSIZE | R_DISABLED`. Its error tells why: `ENOSYS` (no io_uring in the kernel or sandbox), `EPERM` (`kernel.io_uring_disabled` = 2, or 1 outside `kernel.io_uring_group`, or seccomp), `EINVAL` (kernel older than 6.1), `ENOMEM` / `EMFILE`.
2. `IORING_REGISTER_PROBE`: `RECV`, `ACCEPT`, `POLL_ADD`, `ASYNC_CANCEL`; features `NODROP`, `EXT_ARG`, `FAST_POLL`.
3. Buffer ring, restrictions ([Security posture](#security-posture)), registered ring fd, io-wq cap; then enable the ring.

A failure closes the ring, logs once per server (info for `ENOSYS` and `EPERM`, warning otherwise), counts in the statistics, and the run uses epoll. No switch during a run. During the run, `io_uring_enter` failing with `EINTR`, or with `EBUSY` (completions kept by `NODROP` after a completion queue overflow, reaped first [doc]), is retried; any other failure stops the server, as a failing `epoll_wait` does. `CQSIZE` covers one recv per connection, so that an overflow stays exceptional.

**Runtime field**: `HttpServerConfig::ioBackend` (`IoBackend::Epoll` by default, `IoBackend::IoUring` when available), always present like `zerocopyMode`, ignored with one log line without the option, applied at the next run. A field, not only a build option: operators turn the ring off without rebuilding, and benchmarks A/B both backends in one binary, keeping code layout out of the comparison.

**Build**: `AERONET_ENABLE_IO_URING` (OFF, Linux only), on the raw UAPI (`<linux/io_uring.h>`, `syscall()`): about 300 lines (ring mmaps, SQE and CQE access with acquire / release on the indexes, buffer ring updates, 5 operations), no new dependency for FetchContent, Conan and vcpkg, the barrier code covered by unit tests. liburing (dual MIT / LGPL-2.1 [doc]) would bring ready helpers but a dependency and header/library version skew; it stays in the throwaway probes. Ubuntu 24.04's `linux-libc-dev` 6.8 defines every constant used, lacking only later ones (`IOU_PBUF_RING_INC`, `IORING_RECVSEND_BUNDLE`, `IORING_OP_EPOLL_WAIT`), checked in the installed header.

#### Security posture

- **Users who restrict io_uring see no change**: the default is epoll, and a refused setup falls back with one log line. Restrictions are common: Docker's default seccomp profile blocks the io_uring syscalls since Docker 25 [doc], containerd's too (version (?)), Kubernetes `RuntimeDefault` uses the runtime's; Google disabled io_uring on ChromeOS, Android apps and its production servers (2023, after most kernel exploits submitted to its bug bounty used it) [doc]; RHEL 9 reportedly ships it disabled (?); kernels from 6.6 have `kernel.io_uring_disabled`.
- **Users who enable it accept a larger kernel attack surface** for their whole container or host, not only aeronet: the io_uring syscalls allowed in seccomp, `kernel.io_uring_disabled` = 0 (or 1 with the group). The documentation must say so.
- **Visibility changes**: ring operations bypass per-syscall seccomp filters and syscall-based auditing (audit rules, sensors on syscall tracepoints): a connection accepted or read through the ring shows no `accept4` or `recvfrom`. The socket-layer LSM hooks still run [assumed: io_uring calls the same socket functions]. Open question 5.
- **Hardening**: the ring is created disabled (`R_DISABLED`), restricted to its operations (`IORING_REGISTER_RESTRICTIONS`, 5.10: `RECV`, `ACCEPT`, `POLL_ADD`, `POLL_REMOVE`, `ASYNC_CANCEL`), then enabled: a memory corruption in the process cannot turn it into an `openat` or `connect` primitive. No SQPOLL, no registered buffers (nothing charged to `RLIMIT_MEMLOCK`; ring memory goes to the memory cgroup since 5.12 [doc]).

#### Testing

- **AddressSanitizer does not see kernel writes** [probe, Q3]: with a provided buffer freed while in the ring, the kernel wrote 12 bytes into the freed memory unreported; only the later user-space read was reported, and that report crashed inside ASAN, the kernel having overwritten ASAN's record of the free. Buffer lifetime must hold by construction: one slab owned by the ring object, never freed while registered, the teardown order of [Buffer ownership and lifetime](#buffer-ownership-and-lifetime), an assertion that nothing is in flight before the free. In ASAN builds, buffers given back to the ring are poisoned (`ASAN_POISON_MEMORY_REGION`) and unpoisoned on completion, so a user-space access to a buffer the kernel owns is reported.
- **ThreadSanitizer**: kernel accesses are invisible to it, and a single-issuer ring has no user-space concurrency. The wakeup keeps its libc `read()` ([Threads](#threads)), the ring indexes use `std::atomic_ref` acquire / release. The TSAN job gets a ring variant for the wakeup paths.
- **CI kernels**: the runners in use are `ubuntu-24.04` (x86_64, arm) and `ubuntu-26.04` (`ubuntu-latest`), whose job logs do not print the kernel: Azure kernels of 6.8 or later with io_uring enabled are likely [assumed]. Stage 0 prints `uname -r`, `kernel.io_uring_disabled` and the setup result. The Alpine job runs in a Docker container (API 1.54 on the runner), whose default seccomp profile should refuse io_uring [doc]: the fallback gets exercised there.
- **New job**: one Linux Debug job (ASAN, features on) runs the whole suite with the ring forced by a test hook (an environment variable read by the test support, like `ScopedTransportDecorator` for transports); existing jobs keep testing epoll.
- **Existing fault injection**:
    - `TestTransport` / `TestPipe` (no event loop): not concerned.
    - `FaultInjectingTransport` through `ScopedTransportDecorator` ([network-fault-injection_test.cpp](../tests/network-fault-injection_test.cpp)): a decorated transport is not plain, so it stays on epoll and those tests keep testing what they test. Putting the ring input under the transport, for decorators to wrap, would grow `Transport` (asserted at 3 pointers) and put ring state into `aeronet/sys` transports: not proposed.
    - The libc interposers of `sys-test-support.hpp` (41 test files): output injections (`SetSendmsgActions` used 11 times, `SetSendfileActions` 5, `SetWriteActions` 4, and `AcceptInstallActions` 11, which the `accept4` interposer installs on the fd it accepts) still apply to ring connections in stage 1, whose accepts and output stay on libc; stage 2 must pin epoll for them and for the tests reading `g_last_accepted_fd`. Input ones (`SetEpollWaitActions` 12, `SetReadActions` 9, `FailAllEpollCtlMod` 6, `PushRecvAction` 5) do not see ring operations: those tests pin `IoBackend::Epoll`.
    - **Ring equivalent**: a weak callout around `io_uring_setup()` (like `AeronetSslSetFd()`) to fail the setup with `ENOSYS`, `EPERM` or `EINVAL`; and a completion hook at the dispatch boundary, driven by `FaultPolicy` where its fields apply: short counts, `-ENOBUFS`, `-ECANCELED`, `-ECONNRESET`, `SOCK_NONEMPTY` flipped, a completion delayed to the next iteration, a stale completion after close and fd reuse, a late one after a detach to epoll, CQ overflow. It shortens or delays data, never drops it, the limit [Testing](development/testing.md) notes for simulated `EAGAIN`.

#### Staged plan and stop criterion

Every A/B runs one binary with both `ioBackend` values, 5 alternating rounds, reporting mean, min and max, on the scenarios of `benchmarks/scripted-servers` plus churn (`Connection: close` through a Lua script). Hardware: the CI benchmark topology on GitHub-hosted runners (server on CPU 0, wrk on CPUs 1 and 2, several jobs to sample the runner CPU types) and the development machine.

| Stage | Content | Exit criteria |
| --- | --- | --- |
| **0. Measure** (no library code, a few days) | On a throwaway branch: the probe against today's best epoll loop (no `epoll_ctl(DEL)` before `close()`), churn paying `getpeername()` (multishot accept cannot return each peer address [doc]), a `workflow_dispatch` job printing kernel, sysctl, setup result and CPU model. On `main` meanwhile: skip `epoll_ctl(DEL)` before `close()` (+0.6% to +1.1% churn measured), `TimerFd::drain()` stopping after one read | Results per runner CPU type recorded in this section. **Go** only if, on the median CPU type, the ceiling reaches +8% requests/s on HTTP/1.1 plain keep-alive at 100 connections, or +12% connections/s on churn, with the 8-connection run not below -2%. Otherwise stop: results recorded here, item closed as measured |
| **1. Ring wait and ring input** | Option, runtime field, `internal::RingInput`, nested epoll, fallback, restrictions, completion hook, CI job, docs (feature page, CMake option and configuration references, flag matrix) | Suite green on both backends under ASAN and TSAN; fallback tests for `ENOSYS`, `EPERM`, `EINVAL`; 100% coverage of new code; idle heap per connection unchanged (2,035 B at 10k idle plain connections, within 5%); at most 1.05 syscalls per request on HTTP/1.1 plain keep-alive at 100 connections; the stop criterion below |
| **2. Multishot accept** | Plain listeners; peer address by `getpeername()` or lazily; a bound on accepted but unserved connections, keeping the backlog pushback of `maxAcceptBatchSize`; cancellation before the listener closes | Churn +5% over stage 1, same guards; tests relying on the `accept4` interposer pinned to epoll |
| **3. Candidates**, each gated | Registered files (+1%); `SEND_ZC` between two hosts (CPU per GiB -10%); TLS memory BIO (+5% on HTTP/1.1 user-space TLS at 100 connections beyond the epoll write BIO); incremental buffers on 6.12+ (memory at 10k connections); `IORING_OP_EPOLL_WAIT` on 6.15+ (if it removes code) | As stated |

**Stop criterion.** Stage 1 is not merged unless, on the CI benchmark runners, the ring gives at least **+5% requests/s on HTTP/1.1 plain keep-alive `static` at 100 connections and +5% connections/s on churn at 100 connections** (mean of 5 alternating rounds, every ring round above every epoll round), and no guard fails: no other scripted scenario (HTTP/1.1, h2c, TLS; 10 and 100 connections) more than 1% below epoll, no p99 latency more than 5% above, the 1 and 8-connection runs at most 2% below, and behind a same-host nginx (keep-alive upstream, `/ping` then a 25 µs handler) server CPU per request no higher than with epoll. Below that, the branch stays unmerged, for reference.

#### Open questions

1. **The ceiling on CI-class hardware**: stage 0, on several runner CPU types (the kTLS check of 2026-11-18 met Xeon 6975P-C, EPYC 9V45 and EPYC 7763).
2. **Runner kernels and io_uring availability**, Alpine container included: the stage 0 job.
3. **The buffer pool at 10k active connections** (`-ENOBUFS` rate, memory without 6.12's incremental consumption): the 10k-connection scenario of the network cost study on the stage 1 build, deciding between a capped pool and requiring 6.12.
4. **Close timing**: may every close release its socket at the next wait (FIN, or RST of an abortive close, microseconds later), or must cancellations be submitted at once (one syscall per close, like `epoll_ctl(DEL)` today)? The lingering and zerocopy close tests on the stage 1 build, and a churn A/B of both.
5. **Security tooling**: do the LSM and BPF sensors operators run (like the one on the development machine) see ring socket operations, and do their policies flag io_uring? Not resolvable on a managed laptop: ask users, read vendor documentation.
6. **Defaults of RHEL 9, containerd, Podman, gVisor**: only the fallback depends on them; run the stage 1 fallback test in their images.
7. **IOCP**: a Windows IOCP backend sharing a completion-based core would change the cost of seam (b). Decide the IOCP item of [Low priority](#low-priority) first.
8. **aeronet's bookkeeping against the probe's ceiling** (slot lookup, copy, arming): the stage 1 A/B with the network cost study's cycle and syscall counters.

#### Kernel features

| Feature | Kernel | Use | Status |
| --- | --- | --- | --- |
| Setup, enter, register; `RECV`, `ASYNC_CANCEL`, `POLL_ADD`, `REGISTER_PROBE`; `NODROP`; `FAST_POLL`, `SPLICE` | 5.1 to 5.7 | Stage 1 | [doc] |
| `R_DISABLED`, `REGISTER_RESTRICTIONS`, `ENABLE_RINGS`; `ENTER_EXT_ARG` | 5.10; 5.11 | Stage 1 | [doc] |
| Multishot `POLL_ADD` | 5.13 | Nested epoll | [probe] one completion per new readiness of an epoll fd |
| `IOWQ_MAX_WORKERS`; `CQE_SKIP_SUCCESS`; `SUBMIT_ALL`, `REGISTER_RING_FDS` | 5.15; 5.17; 5.18 | Stage 1 | [doc] |
| Provided buffer rings, multishot `ACCEPT`, `RECVSEND_POLL_FIRST`, `CQE_F_SOCK_NONEMPTY`, `ASYNC_CANCEL_ANY` | 5.19 | Stages 1, 2 | [probe] buffers taken on arrival, `-ENOBUFS` when empty |
| `SINGLE_ISSUER`, multishot `RECV`, `SEND_ZC`, `REGISTER_SYNC_CANCEL` | 6.0 | Stage 1; rejected; stage 3; option | [doc] |
| `DEFER_TASKRUN` | 6.1 | Minimum kernel | [probe] accepted by 7.0 with `SINGLE_ISSUER` |
| `TIMEOUT_MULTISHOT` | 6.4 (?) | Not planned | [doc] |
| `kernel.io_uring_disabled`, `kernel.io_uring_group` | 6.6 | Fallback | [doc]; 0 on the development machine |
| `RECVSEND_BUNDLE`; `IOU_PBUF_RING_INC` | 6.10; 6.12 (?) | Not planned; stage 3 | [doc] |
| `IORING_OP_EPOLL_WAIT`, zero-copy receive | 6.15 (?) | Stage 3; not planned | [doc] |
| An armed recv after `close(fd)` | - | Close ordering | [probe] the socket lives, no FIN, until the recv is cancelled |

#### Probes

Throwaway programs, not in the repository: liburing 2.5, kernel 7.0.0-43, `SINGLE_ISSUER | DEFER_TASKRUN`. The 2026-11-10 study measured the throughput table of the baseline and checked `TCP_NODELAY` inheritance from the listener, multishot poll on an epoll fd, and an armed recv after `close()`. The probe of 2026-11-18 (about 200 lines) answered three questions:

| Question | Decides | Answer |
| --- | --- | --- |
| Q1. Can a completion name an fd number that was closed and given to a new connection? | Generations in `user_data` | Yes: the recv armed on the closed fd 6 completed after a new connection got fd 6, with the old peer's bytes |
| Q2. Do idle armed recvs with buffer selection pin provided buffers, and what does a recv get from an empty ring? | Pool sizing, overload path | Nothing is pinned: 64 idle recvs on 4 buffers, no completion; buffers taken on arrival; an empty ring completes the recv at once with `-ENOBUFS`, the data left in the socket (a direct `recv()` read it) |
| Q3. Does ASAN report the kernel writing into a provided buffer the program freed? | Whether sanitizers police buffer lifetime | No: the kernel write went unreported, only the later user-space read was, and that report crashed inside ASAN, whose record of the free the kernel had overwritten |

### Asynchronous reads of cold files

Status: design study of 2026-11-18, made with the [`io_uring` backend](#linux-io_uring-backend-design-study) one; nothing is implemented. That one is about throughput, this one about latency: while one connection waits for the disk, every connection of its loop waits too. It does not need the network backend, and io_uring is one of two ways to address it. Markers as in the [`io_uring` study](#baseline-and-evidence).

**What blocks today** [code], all on the loop thread (opening files with `O_NONBLOCK` only protects against FIFOs, see `File`):

- `StaticFileHandler::operator()`: `std::filesystem::status()`, then `File` open (`open` + `fstat`), then for files up to `inlineFileThresholdBytes` (128 KiB) a `File::readAt()` (`pread`) into the response body, inside the handler; multipart range bodies (`BuildMultipartBody()`, up to `maxMultipartBodySize`, 1 MiB) read the same way.
- Larger files and single ranges, as a `FilePayload`: `ConnectionIo::flushFilePayload()`, with `sendfile()` in 2 MiB chunks (plain) or `pread()` of 128 KiB then `SSL_write` (TLS).
- HTTP/2: `Http2ProtocolHandler::sendPendingFileBody()`, `readAt()` of up to 256 KiB per chunk within the flow-control windows.

**How long a miss blocks** [probe, F1]: a 64 MiB file on ext4 (laptop NVMe behind dm-crypt), evicted with `POSIX_FADV_DONTNEED` before each cold read, 21 reads per row. Directory and inode caches stay warm, so cold `open` / `stat` are not measured.

| Call | Cold, median (p90) | Warm, median |
| --- | ---: | ---: |
| `pread` 16 KiB | 83 µs (96) | 0.9 µs |
| `pread` 128 KiB (inline threshold) | 176 µs (183) | 6.7 µs |
| `pread` 2 MiB | 778 µs (866) | 54 µs |
| `sendfile` 256 KiB to a socket | 440 µs (957) | 12 µs |

At 200k requests/s, 0.8 ms is the loop time of 160 requests. Cloud block storage and network filesystems are slower than a local NVMe [assumed].

**Detecting a miss without blocking** [probe, F2]:

- `preadv2(RWF_NOWAIT)` failed with `EAGAIN` on 21 of 21 cold 128 KiB ranges, after 16 µs (it seems to start the read-ahead [assumed]). On a hit it is the read itself, so the `pread()` of the inline, TLS and HTTP/2 paths can become a `preadv2(RWF_NOWAIT)` with no extra syscall when the data is cached; a partly cached range gives a short read, the rest being read asynchronously.
- `cachestat()` (6.5) reported 21 of 21 cold ranges as not cached, in 0.1 to 0.5 µs: the detector of the `sendfile()` path, which has no `RWF_NOWAIT`, one call per 2 MiB chunk. Before 6.5, `sendfile()` stays synchronous.
- Path lookups: `openat2()` with `RESOLVE_CACHED` (5.12) fails with `EAGAIN` when the lookup needs I/O [doc]; whether cold lookups matter is for F0 below.

**Two ways to read asynchronously.**

| | io_uring file reads | Thread pool |
| --- | --- | --- |
| Loop stall per miss | 16 µs for 128 KiB: the submission starts the I/O in the loop thread [probe, F3]; `IOSQE_ASYNC` would move it to an io-wq worker [doc] | A queue push and a futex wake, a few µs [assumed] |
| Threads | None on ext4: async buffered reads, no io-wq worker seen [probe, F3]; io-wq workers on file systems without them [doc] | Worker threads |
| Completion to the loop | An eventfd registered with `IORING_REGISTER_EVENTFD` (5.2) in the existing epoll, or the network ring of the [`io_uring` backend](#seams-compared) if it exists | The cross-thread path that already exists: a queue under a mutex, then `wakeupFd`, as async handler resumes do |
| Requirements | Async buffered reads (5.9 (?)), io_uring allowed ([Security posture](#security-posture)) | None, every platform |
| Shared with | The ring wrapper of the [`io_uring` backend](#seams-compared) | The [thread pool for `deferWork()`](#medium-priority) |

A miss costs 80 µs to 0.8 ms of disk time, against which a few µs of thread handoff do not matter, and the pool needs no kernel feature, no relaxed seccomp profile, and serves `deferWork()` too. So: an asynchronous file read interface with a pool behind it first, and a ring backend only if it beats the pool on CPU per miss at high miss rates.

**Design.**

- **Interface**: an `internal::FileReader` component in `ServerContext`: a read is submitted with the connection's fd and generation, the file, an offset and a length; its completion (bytes or error) is dispatched on the loop thread.
- **Ownership**: while a read is in flight, its buffer and its `File` belong to the read, not to the connection. A close only orphans it (generation check, as in [Buffer ownership and lifetime](#buffer-ownership-and-lifetime)), and its completion frees both. A pool worker reads through the fd number, so the `File` must stay open until the read completes: closed, its number could be reused by another open, and the worker would read another file. A ring holds its own file reference.
- **Inline files**: on `EAGAIN`, the handler does not wait: it answers with a file payload for the whole file, as for a large one, so the handler stays synchronous and the asynchronous read happens in the output path. Responses compressed in the handler (small text files) need their body there: they keep a synchronous read in F1 (open question below).
- **HTTP/1 file payloads**: on a miss (`cachestat()` before `sendfile()`, `EAGAIN` before a TLS `pread()`), `flushFilePayload()` submits the read of the next chunk and pauses the connection's output, like a writable wait. The completion hands the buffer to `tunnelOrFileBuffer`, written by the path user-space TLS already uses: for plain connections that chunk loses zero copy, against a disk read. The next chunks, warmed by the read-ahead, go back to `sendfile()`.
- **HTTP/2 file bodies**: on a miss in `sendPendingFileBody()`, that stream waits for its chunk while the other streams go on. The HTTP/2 layer gains a pending file read per stream, and knows the `FileReader` interface only, never the backend.
- **Bounds**: one read in flight per file send, chunks of 256 KiB at most, a configured limit of bytes in flight per loop, beyond which reads stay synchronous (today's behavior). A connection waiting for its file is bounded by `keepAliveTimeout` without progress, as a writable wait is, so a hung network filesystem cannot pin it forever.
- **Tests**: interposers for `preadv2()` (`EAGAIN`, short reads) and a weak callout for `cachestat()`, completions delayed and reordered, a close while a read is in flight, an fd number reused meanwhile, a failing reader. Real misses need a disk file system, and the tests run with `TMPDIR=/dev/shm` (tmpfs, where `POSIX_FADV_DONTNEED` evicts nothing): they must not rely on them, the F0 harness does. The pool is ordinary threads, which ThreadSanitizer covers.

**Plan and stop criterion.**

| Stage | Content | Exit criteria |
| --- | --- | --- |
| **F0. Measure** | Harness: one loop, wrk on `/ping` at 100 connections (hot) plus a client fetching files evicted before each request, at 1, 10 and 100 per second, of 16 KiB, 128 KiB and 2 MiB; hot p99, p99.9 and max. On the development machine, and on GitHub-hosted runners where `sudo` can drop the caches, dentries included (cold `open` / `stat`) | If the hot p99.9 at 10 cold requests per second stays within twice the hot-only run, stop: document the trade-off (warm caches, a CDN) |
| **F1. Pool, HTTP/1** | `FileReader` with its pool, `RWF_NOWAIT` reads, `cachestat()` before `sendfile()`, inline misses turned into file payloads, statistics (cold reads, bytes) | The stop criterion below |
| **F2. HTTP/2** | Pending file reads per stream | Same criterion over h2c |
| **F3. Ring backend** | io_uring `FileReader` | CPU per miss 20% lower than the pool at 100 misses per second, same hot latency |
| **F4. Cold lookups** | `RESOLVE_CACHED`, then `open` / `stat` through the reader | Only if F0 shows cold lookups matter |

**Stop criterion**: F1 is merged only if, with 10 and with 100 cold-file requests per second on the loop, the p99.9 latency of the hot requests is at least halved, the warm `files` scenario of `benchmarks/scripted-servers` stays within 1% of today, and the throughput of cold files is not lower.

**Open questions**: the latencies of real storage (cloud block storage, network file systems, container overlays), from F0 on the runners and from users; small compressed files (serve the first cold request synchronously, or compress in the output path); the pool size, and whether one pool serves file reads and `deferWork()` for the whole process; macOS and Windows, which have no `cachestat()` (`mincore()` needs a mapping): offload every read above a size, or none.

**Kernel features**:

| Feature | Kernel | Use | Status |
| --- | --- | --- | --- |
| `preadv2(RWF_NOWAIT)` | 4.14 (?) | Miss detection | [probe] `EAGAIN` on cold ranges |
| `IORING_REGISTER_EVENTFD`; `READ`, `OPENAT`, `STATX` | 5.2; 5.6 | Ring backend | [doc] |
| Async buffered reads | 5.9 (?) | Ring backend | [probe] no io-wq worker for cold reads on ext4 |
| `openat2(RESOLVE_CACHED)` | 5.12 | F4 | [doc] |
| `cachestat()` | 6.5 | Miss detection | [probe] cold ranges reported, 0.1 to 0.5 µs |

**Probes**: a throwaway program of 2026-11-18 (about 250 lines, a 64 MiB file on ext4, pages evicted with `POSIX_FADV_DONTNEED`):

| Question | Decides | Answer |
| --- | --- | --- |
| F1. How long does a page-cache miss block `pread()` and `sendfile()`? | Whether the track is worth it | 83 µs (16 KiB) to 778 µs (2 MiB), 440 µs for a `sendfile()` of 256 KiB, against 1 to 54 µs warm |
| F2. Do `preadv2(RWF_NOWAIT)` and `cachestat()` detect a miss without blocking, and at what cost? | Miss detection on the hot path | Both detected 21 of 21 cold ranges; `RWF_NOWAIT` returned `EAGAIN` after 16 µs and is the read itself on a hit; `cachestat()` took 0.1 to 0.5 µs |
| F3. Does an io_uring `READ` of a cold range return from the submission at once, and through io-wq? | Ring against thread pool | The submission took 16 µs (the loop still pays the I/O submission), completion 156 µs later; no io-wq worker thread appeared |

## Long-term / Nice-to-have

- **HTTP/3 / QUIC** - a separate transport layer (future major feature); scoped as a research spike below before any commitment.
- **Per-SNI mTLS Policies** - Different client cert requirements per SNI hostname
- **Per-route metrics** - request counts and latency per route

### HTTP/3 / QUIC - research spike

**Not started, and not recommended for the near term.** HTTP/3 is not an increment on the existing stack: it
replaces the transport. QUIC is UDP + user-space loss recovery / congestion control + connection migration +
TLS 1.3 fused into the transport, and it uses **QPACK, not HPACK** - so the current HPACK / frame codecs and
the readiness-based epoll/kqueue `TlsTransport` seam mostly do not carry over (QUIC is more naturally
timer/completion-driven and wants GSO/GRO for throughput). Writing QUIC from scratch is a multi-year,
security-critical effort; the realistic path is wrapping an existing stack behind a new gated transport
module, which trades against aeronet's minimal-heavy-dependency ethos and therefore needs a deliberate
decision rather than a default.

**Higher-leverage work comes first** and reuses the existing architecture: the coroutine client + true HTTP/2
multiplexing, and [`io_uring`](#linux-io_uring-backend-design-study).

If it becomes strategic, the first step is a time-boxed **design spike** (not a full plan) answering three
questions:

1. **Dependency choice** - `ngtcp2` + `nghttp3` (the curl/nginx pairing), `quiche`, or `lsquic`: build system,
   license, binary-size and API-ergonomics trade-offs.
2. **Event-loop integration** - how a UDP + timer-driven QUIC transport slots into the current readiness-based
   loop (UDP socket handling, per-connection timers, GSO/GRO, pacing).
3. **Handler API reuse** - whether the existing `HttpRequestView` / `Router` / handler surface can sit unchanged on
   top of an HTTP/3 mapping (QPACK encode/decode, request/response streams), so HTTP/3 is a transport swap
   rather than a second application API.

Deliverable of the spike: a go/no-go recommendation with a dependency choice and a rough transport-layer design, **before** committing to implementation.

### Optional feature modules (compile-time gated)

The following features are inspired by capabilities that make frameworks like **Drogon**, **Crow**, and **Beast** popular for building full applications. Each is designed to be **opt-in via a compile-time flag** (`OFF` by default) so they add zero overhead when unused, consistent with aeronet's modular philosophy.

#### HTTP Client (`AERONET_ENABLE_HTTP_CLIENT`)

Delivered, see the [HTTP client guide](modules/http-client.md). Still planned (receive sizing and DNS/TLS reuse are in
the [performance optimization backlog](#performance-optimization-backlog)):

- **Coroutine-friendly API** (`co_await client.get(...)`) integrated with a *running* server event loop, so handlers can issue upstream calls without blocking. This is also what unlocks **true HTTP/2 multiplexing** in the client: the native HTTP/2 engine has landed, but the synchronous model runs one stream at a time per connection.
- **Unify the request buffer with the wire bytes**: teach `HttpMessage` a request-line first-line mode so the client builds the request directly in the buffer it writes, with zero reassembly (today request fields are copied once into the send buffer).
- **Cancellation from another thread** of an in-flight client exchange (a `ResponseBodySink` can already stop a transfer, and `inactivityTimeout` bounds a stalled one).
- **Client-side telemetry**: surface pool hit/miss, retry counts, cache hits/misses, redirect hops and decompression stats through the same `TelemetryContext` infrastructure the server already uses (the client currently emits none).
- **Cookies, next steps** (after the [delivered](modules/http-client.md#cookies) in-memory jar):
  - import and export of the Netscape `cookies.txt` format (curl, wget, yt-dlp, browser extensions), so that a long-running client keeps its session across restarts and can start from the cookies of a browser;
  - the public suffix list, behind the heuristic of `RegistrableDomain()` (`aeronet/client/src/cookie-jar.cpp`): an optional libpsl (vcpkg, Conan, Linux systems) for exact registrable domains (`github.io`), and same-site checks on registrable domains instead of hosts.
- **Reverse-proxy / forwarding** building on the client (see the reverse-proxy item below).
- **WebSocket client, next steps** (after the [delivered](protocols/websocket-client.md) `WebSocketClient`):
  - automatic reconnection with the backoff fields of `RetryConfig`, `onOpen` running again to replay the subscriptions, and a minimum delay between the connection attempts to a host (some CDNs ban an IP for minutes after about 150 attempts in 10 minutes);
  - a pull-style `receive(timeout)` for request / response protocols such as CDP;
  - sends from other threads (an eventfd waking `poll()`), and the event loop handle exposed so that a client can sit inside an outer event loop;
  - a non-blocking opening handshake, so that a reconnecting connection does not hold the others.

#### HTML parsing and JavaScript pages (`AERONET_ENABLE_HTML`, `AERONET_ENABLE_JS`)

Status: [HTML parsing and DOM mutation](modules/html.md) (phase 1) and the
[client cookies](modules/http-client.md#cookies) (phase 2) delivered, phase 3 started: its first part, the
[page runtime](modules/web.md) without `fetch` and XHR, delivered
([#738](https://github.com/sjanel/aeronet/issues/738)). Goal: let the client read pages whose content is produced or
completed by JavaScript, e.g. `page.querySelectorAll("table tr")` after a `navigate()`.

**The JS engine is the small part.** Embedding one is a few C files; the DOM, the script loading rules, the event
loop and the Web APIs (`fetch`, XHR, timers, `location`, storage, observers...) are most of the work, and that
surface is never complete: Lightpanda, a funded headless browser built on V8, still describes its Web API coverage
as partial. On the other hand many "JavaScript pages" do not need JS at all: they are rendered on the server, or
they embed their data as JSON that the page scripts turn into markup, which phase 1 reads (the
[Kraken withdrawal fees page](https://support.kraken.com/articles/360000767986-cryptocurrency-withdrawal-fees-and-minimums)
of #738 has no `<table>`: its 799 rows are in `window.__INITIAL_PROPS__`, behind 51 scripts).

Worked example, a [Nintendo Store product page](https://store.nintendo.com/fr-fr/manette-pro-nintendo-switch2-edition-40e-anniversaire-de-the-legend-of-zelda-support-dexposition-000000000010019437)
watched for its stock (steps 1-3 checked on 2026-11-07, step 4 from a headless Chromium dump of 2026-11-02). Its
data needs no JS, but reaching it does: the store is behind a Queue-it waiting room that sends every new visitor
through a script.

1. A request without cookies is answered `307` to `nintendostoreuk.queue-it.net`.
2. That page (2.4 KB) has one inline script: it checks `navigator.cookieEnabled` and `document.cookie`, then sets
   `document.location.href` to a relative URL carrying a signed timestamp. Its `<noscript>` says "The site requires
   JavaScript".
3. When no queue is running, that URL answers `302` back to the product page, with a signed `queueittoken` in the
   query and in a `Queue-it-token` cookie (3 minutes, `SameSite=None`).
4. The product page (1.25 MB, Next.js) holds the stock state in `<script id="__NEXT_DATA__" type="application/json">`
   (1 MB): `props.pageProps.product.inventory.orderable`, `c_availabilityModel.type` (`"OutOfStock"`), the price.
   Its ~25 scripts (Next.js chunks, consent, account SDK, analytics) are not needed. Searching the HTML for
   "rupture de stock" finds it whatever the stock: the words are in the translations of `__NEXT_DATA__`.

What it needs, in the phases below: a cookie jar across sites (phase 2, delivered: steps 1 to 3 already keep and send
the cookies of both sites), script navigation, parsing in scripting mode, and a selector checked before the scripts
of each document run (phase 3). When a queue is running, step 3 is
a real waiting room (it polls Queue-it, and may ask for a CAPTCHA or a proof of work): out of scope like the other
anti-bot challenges, the client does not try to skip it, `navigate()` ends at its budget on the queue-it.net
document.

Dependencies (C libraries, kept out of the public headers):

- **lexbor** (Apache-2.0, already used by phase 1): its per-element mutation callbacks (insertion, removal,
  attribute change) are the hook for scripts inserted at runtime and `MutationObserver`; each node has a `user`
  pointer to cache its JS wrapper (the one of the document node holds the query engine of `HtmlDocument`); a
  document can be parsed in scripting mode (`lxb_html_document_scripting_set`).
- **QuickJS-ng** (MIT, ES2023+, CMake, MSVC, vcpkg): not in Conan Center, which only has Bellard's QuickJS -
  either contribute a recipe or support both behind a thin layer (their C APIs are close). No `Intl` (the
  project says it will likely never have it, so i18n libraries need stubs), no WebAssembly, interpreter speed.
  V8, SpiderMonkey and JavaScriptCore are rejected for binary size and build complexity; Duktape, MuJS and
  JerryScript cannot parse modern bundles (ES5).

Phases:

1. **HTML** - delivered, with DOM mutation ([Modifying the tree](modules/html.md#modifying-the-tree)): attributes,
   text, `setInnerHtml()` and `insertAdjacentHtml()` (fragments parsed with the limits of a document), element
   creation, moves and removal; no node is freed before its document, which is the node lifetime phase 3 needs. Phase
   3 will need handles on text and comment nodes, which the C++ API does not expose.
2. **Client cookie jar** - delivered, see [Cookies](modules/http-client.md#cookies): RFC 6265bis storage and
   retrieval, `SameSite` along redirect chains across sites, merged with the `Cookie` header of the request.
3. **JS core** (`AERONET_ENABLE_JS`, requires `AERONET_ENABLE_HTML` and `AERONET_ENABLE_HTTP_CLIENT`), module
   `aeronet/web`: `WebClient::navigate(url, options)` returning a `std::expected`, the same `Element` API as phase 1,
   and `evaluate()`. The first part is [delivered](modules/web.md): everything below but `fetch`, XHR and the
   `SameSite` context of the subresources. Left for the next part:
   - `fetch` and `XMLHttpRequest` over the `HttpClient` of the page, and `LoadState::NetworkIdle` waiting for the
     requests in flight, not only for the timers;
   - the subresources of a page as requests of its site for the cookie jar (a cross-site `<script src>` gets the
     `SameSite=Strict` cookies of its site today);
   - a per-client bytecode cache of the page scripts keyed by URL and content (the prelude of the Web APIs is already
     compiled once per client: 180 us instead of 760 us per page setup);
   - the fragment of a `Location` header, which `HttpClient` drops (the page keeps the one of the requested URL).

   The design points of the first part:
   - One QuickJS runtime per page over the lexbor document; the page owns every node and frees them together,
     so node lifetime does not depend on the JS garbage collector.
   - Documents are parsed in scripting mode, as browsers with JS do: `<noscript>` content is text, not markup.
   - Scripts run in document order with `defer` semantics after a full parse (`document.write` during parsing
     is limited).
   - Script navigation (`location.href`, `location.assign()` / `replace()`, `<meta http-equiv="refresh">`) drops
     the page and its runtime and loads the new URL, resolved against the document URL; the hops count toward a
     limit, like redirects.
   - Event loop: one task, then the microtasks (`JS_ExecutePendingJob`). Timers use a virtual clock: they fire
     in order without sleeping, with caps on far timers and on `setInterval` runs.
   - `navigator` has `cookieEnabled`, `userAgent` and `language`. `document.cookie` reads and writes the jar of
     the client as a non-HTTP API (`HttpOnly` cookies hidden and kept, RFC 6265bis §5.7).
   - `navigate()` returns on a load state, on a matching selector, or when the time budget runs out (QuickJS
     interrupt handler). The selector is checked when a document is parsed, before its scripts run: data served in
     the HTML costs no script run.
   - Each node wrapper is cached in the `user` pointer of its lexbor node and kept until the page dies, so that
     `a === b` holds and the properties that scripts add to nodes stay (React does this).
4. **Single-page app compatibility**: ES modules and `import()`, scripts inserted at runtime (webpack chunk
   loading) through the lexbor mutation callbacks, `MutationObserver`, stubs (`Intl`, `matchMedia`,
   `getComputedStyle`, `IntersectionObserver` reporting everything visible, layout sizes of 0),
   `waitForSelector()`, then clicks and form input.
5. **Optional, independent**: a driver for full-fidelity browsers (Chrome, Lightpanda) over CDP (JSON over
   WebSocket). Little aeronet code, on top of the [WebSocket client](protocols/websocket-client.md), which needs a
   pull-style `receive(timeout)` for its request / response exchanges.

```cpp
aeronet::WebClient web;  // its cookie jar outlives navigate()

// A single-page app: run the scripts until the network is idle.
auto page = web.navigate(url, {.until = aeronet::LoadState::NetworkIdle, .timeout = std::chrono::seconds{10}});
if (page) {
  auto rows = page->document().querySelectorAll("table tr");
}

// The Nintendo Store page above: 4 exchanges and the waiting-room script, none of the product page scripts.
auto product = web.navigate(nintendoUrl, {.untilSelector = "script#__NEXT_DATA__", .timeout = std::chrono::seconds{10}});
if (product) {
  std::string nextData = product->document().querySelector("script#__NEXT_DATA__").textContent();  // parse with glaze
}
```

Scope: server-rendered pages with JS on top, and single-page apps (React, Vue, Svelte, webpack) that only need the
DOM, `fetch` / XHR and timers. Out of scope: rendering and layout, anti-bot challenges (they fingerprint canvas,
WebGL and timing), WebAssembly, workers, iframes, canvas, media.

Performance: the synchronous client fetches the subresources one at a time (51 sequential exchanges for the Kraken
page); the coroutine client with HTTP/2 multiplexing (above) is the fix.

QuickJS-ng 0.18.0, measured on 2026-11-19 (Release, clang 23, x86-64 desktop, one core; V8 of Node 24 for
reference):

- **Size**: about 1.1 MB of code (`libqjs.a`, without `quickjs-libc`).
- **Compilation**: about 20 MB/s of minified code (bytecode for every function at once, where V8 compiles lazily and
  is about 5 times faster). The 51 scripts of the Kraken page (2.8 MB) take about 150 ms, its 4.4 MB inline
  `window.__INITIAL_PROPS__ = {...}` 210 ms more (`assignedJson()` reads it in about 1 ms), React 18 `react-dom`
  6 ms. All compiled, the Kraken page holds 22 MB of bytecode and objects.
- **Bytecode cache**: reading back the bytecode (`JS_ReadObject`) is 15 to 70 times faster than compiling (3 ms for
  the 940 KB main bundle, 3 ms for the inline state): a per-client cache keyed by script URL and content would make
  repeated `navigate()` of the same site cheap. Only bytecode written by the process itself can be read: it is not
  validated.
- **Execution**: React 18 `renderToString` of an 800-row table takes 14 ms, against 8 ms for the first run of V8 (its
  JIT warm-up), 5 ms for V8 without JIT, under 1 ms for V8 warmed up. Scripts run once per page, so QuickJS-ng is
  within 2 times of V8 for this use.
- **DOM bindings**: a native getter or method call (class with an opaque pointer) costs about 40 ns more than an
  empty loop iteration, twice a JS function call. The interrupt handler of the CPU budget has no measurable cost.
- **Scripts without a DOM**: with only `window = self = globalThis`, 49 of the 51 Kraken scripts run (webpack chunks
  registering themselves); its runtime chunk stops at `document`.
- **API differences with Bellard's QuickJS**: the interrupt handler takes a `JSContext*` in QuickJS-ng since 0.17, a
  `JSRuntime*` before and in QuickJS: aeronet passes a generic lambda, which converts to both (vcpkg has QuickJS-ng
  0.16.2).

Security: `navigate()` runs remote code in the host process, and QuickJS-ng had memory-safety CVEs in 2025-2026
(typed arrays, `Atomics`, iterator helpers). Never link `quickjs-libc` (no file or OS access); set memory and stack
limits and a CPU budget; add a network policy (allowed origins, request and byte caps); leave out
`SharedArrayBuffer` and `Atomics`; pin the engine version, fuzz the bindings and run the JS tests in the ASAN CI
legs. Document that untrusted sites belong in an unprivileged process; an out-of-process sandbox (seccomp,
landlock) can come later.

Tests: pages served by aeronet's own `HttpServer` (a cookie check and script redirect across two origins, a
webpack-style chunk loader, a vendored production React build rendering a table), so CI needs no internet.

Decisions taken on 2026-11-19: QuickJS-ng in the process before a CDP driver; no JavaScript in the Conan package for
now (vcpkg and the download only); in the process with limits, a sandbox later; `AERONET_ENABLE_JS` on by default as
the main project, like the other features with a dependency; `evaluate()` returns JSON text; the Web APIs that need
no C++ (events, `classList`, `dataset`, `history`...) are a small JavaScript prelude embedded in the library.

**lexbor release to watch** (checked on 2026-11-19: v3.0.1, tagged on 2026-09-07, is still the latest). These fixes
are merged on master but in no release:

- the single-byte (and multibyte) decoders that drop a byte when their output buffer is full (#425, commit 826b121);
- the crash, use-after-free and leaks on allocation failures in document creation and parsing (#448, merged on
  2026-11-11);
- after a failed allocation, the next allocation of a memory pool writes through a null pointer (815b8b2a,
  2026-11-14), as does the HTML tokenizer when its temporary buffer could not grow (#433, 145872a2): with 3.0.1, any
  allocation failure inside lexbor can crash the process later.

With lexbor master (e609d94), every allocation of the DOM mutations, queries and serializations was failed in turn
under ASAN on 2026-11-19, lexbor instrumented with its pool poisoning (`LEXBOR_BUILD_WITH_ASAN`): no memory error and,
once aeronet created the tokenizer of its CSS parsers itself (`MakeCssParser()`), no leak. `lxb_css_parser_init` leaks
the tokenizer it creates when it fails after its creation, still on master: a fix with its test is ready in a fork,
to propose upstream (the workaround stays while lexbor versions without it are accepted). The failures were
injected through `lexbor_memory_setup()`, which works under ASAN, unlike the `malloc` overrides of the tests
(`sys-test-support.hpp` disables them with sanitizers): the ASAN CI legs never run the memory exhaustion tests. When a
release contains all of them:

1. bump the FetchContent URL of `cmake/Dependencies.cmake` and `AERONET_LEXBOR_MIN_VERSION` (an older installed
   lexbor would bring the bugs back), and update the version in [HTML parsing](modules/html.md);
2. remove the workaround of the decoder in `aeronet/html/src/html-encoding.cpp` (input fed in chunks of 1024 bytes
   to a 1032-code-point buffer, so that it never fills), then rerun `LargeUndeclaredDocuments` and the encoding
   tests of `html-document_test.cpp`;
3. fail every allocation of the `HtmlDocument` construction in `html-document_test.cpp` (today only the first one,
   `ExpectBadAllocOnAllocation`), and of the parsing in `test::ExpectBadAllocOnEachAllocation`
   (`aeronet/html/test/html-test-util.hpp`), whose check should also run the operation again after each failure (the
   pool bug only shows on the next allocation); add a test failing the lexbor allocations through
   `lexbor_memory_setup()`, so that the ASAN legs run them;
4. check the lexbor versions of vcpkg (3.0.1) and Conan Center (3.0.0 only, whose `:lexbor-contains()` parsing
   overflows the heap): a package manager still on an old release keeps the workarounds needed.

#### Server-Sent Events (SSE) convenience layer

Thin abstraction over existing chunked streaming - no new compile flag needed. Auto-formats `text/event-stream` content type with `id:`, `event:`, `data:` field framing and `retry:` reconnect hints. API sugar on `HttpResponseWriter`:

```cpp
router.setPath(http::Method::GET, "/events", [](const HttpRequestView&, HttpResponseWriter& w) {
  SseWriter sse(w);
  sse.event("update", R"({"temp": 22.5})");
  sse.event("update", R"({"temp": 23.1})", "evt-002"); // with id
});
```

Growing in popularity as a lighter alternative to WebSocket for server → client push (dashboards, notifications, live feeds).

#### Cookie helpers & Session store (`AERONET_ENABLE_SESSIONS`)

- **Cookie builder**: RFC 6265-compliant `Set-Cookie` helper with `SameSite`, `Secure`, `HttpOnly`, `MaxAge`, `Path`, `Domain` attributes. Currently only raw header parsing exists.
- **Session store**: In-memory session store with configurable TTL and pluggable backend interface. HMAC-signed session cookie for integrity. Both Drogon and Crow include this built-in; it is the most basic state management primitive for web applications.

#### Response caching middleware (`AERONET_ENABLE_RESPONSE_CACHE`)

In-memory LRU cache keyed by method + path + `Vary` headers. Respects `Cache-Control` directives (`max-age`, `no-store`, `no-cache`, `private`). Configurable max entries and memory budget. Per-route opt-in via middleware registration. ETag / `If-None-Match` validation for cached entries. Useful for APIs with expensive computation behind cacheable endpoints.

#### Content negotiation (Accept header)

Parse `Accept` header q-values to select response content type (JSON, YAML, XML, plain text). Return `406 Not Acceptable` when no format matches. Apart from `Accept-Encoding`, and from `makeProtoResponse()` choosing between binary protobuf and ProtoJSON, `Accept` (media type) is left to user code. Frameworks like Spring, Rails, Phoenix, and ASP.NET handle this transparently.

```cpp
router.setPath(http::Method::GET, "/data", [](const HttpRequestView& req) {
  auto negotiated = req.negotiate({"application/json", "text/yaml", "text/plain"});
  // returns best match, or std::nullopt → 406
});
```

#### Request ID / correlation ID

Generate a unique ID per request (UUID or monotonic counter), set it on the response (`X-Request-Id`), and forward it to OpenTelemetry spans. If the client provides `X-Request-Id`, the server adopts it. Essential for distributed tracing and log correlation across microservices. Provided by Nginx (`$request_id`), Envoy, Express (`express-request-id`), Spring (`spring-cloud-sleuth`), FastAPI, etc.

#### IP-based access control middleware

Per-route allowlist/denylist by source IP or CIDR range. Returns `403 Forbidden` for denied IPs. Useful for admin endpoints, internal APIs, and compliance. Nginx (`allow/deny`), HAProxy (`acl`), Caddy, Express (`express-ip-filter`), and Go all support this natively or as first-class middleware.

```cpp
IpAccessPolicy policy;
policy.allow("10.0.0.0/8").allow("192.168.1.0/24").denyAll();
router.setPath(http::Method::GET, "/admin", adminHandler).accessPolicy(std::move(policy));
```

#### Authentication helpers (Basic / Bearer / JWT)

[JWT verification](modules/jwt.md) is delivered. Still planned: a server-side **middleware** that parses the
`Authorization` header (Basic credentials or Bearer tokens) and wires the JWT verifier into a pluggable validator
interface; plus a client-side **JWKS-fetch + cache** helper building on the HTTP client.

#### Reverse proxy / HTTP forwarding mode

Forward incoming requests to upstream backends, rewriting headers (`X-Forwarded-For`, `X-Forwarded-Proto`, `Via`). Builds naturally on the delivered HTTP client module. Load balancing strategies (round-robin, least-connections). This turns aeronet from a pure application server into an edge/gateway server - a common deployment pattern (Nginx, Caddy, Envoy, Traefik, HAProxy).

#### Inbound request body streaming to handler

Deliver chunked request bodies to handlers as they arrive instead of buffering the full payload first. Enables processing uploads in bounded memory. Currently documented as a limitation in the [requests guide](guides/requests.md#limits-and-timeouts). Express (streams), Go (`io.Reader`), Axum (`BodyStream`), Actix-web (`Payload`), and Rust Hyper all expose streaming request bodies.

#### Streaming multipart parsing

Process multipart/form-data uploads part-by-part as data arrives, rather than buffering the entire payload. Critical for large file uploads where the full body does not fit in memory. Currently documented as a limitation in the [requests guide](guides/requests.md#forms-and-file-uploads). Popular in Multer (Node.js), Actix-multipart, Spring `StreamingMultipartResolver`, and Go `multipart.Reader`.

#### Graceful config reload via signal (SIGHUP)

Reload the JSON/YAML config file on `SIGHUP` without restarting the server. Apply mutable config changes (timeouts, limits, compression settings, TLS certs) hot. Pairs with the existing Glaze config loader and `postConfigUpdate()`. Nginx, HAProxy, Caddy, and systemd-based services all support `SIGHUP`-driven reload.

#### WebSocket broadcast / pub-sub helpers

Built-in topic-based broadcast for WebSocket connections. `WsRoom` / `WsTopic` abstraction where connections subscribe to topics and the server broadcasts to all subscribers. Common pattern in Socket.IO, uWebSockets, Phoenix Channels, and SignalR. Reduces boilerplate for chat, dashboards, and real-time notification use cases.

#### ETag support for dynamic responses

Compute a weak or strong ETag from the response body (e.g. hash) and handle `If-None-Match` / `304 Not Modified` for dynamic endpoints, not just static files. Reduces bandwidth for API responses that rarely change. Express (`etag` by default), Rails, ASP.NET, and Spring all auto-generate ETags for regular responses.

## Realistic Network Testing

Goals

- Improve confidence that aeronet behaves correctly under latency, jitter, packet loss, reordering, partial delivery, and connection resets.
- Catch protocol-level bugs (HTTP/1.1 and HTTP/2), TLS handshake fragility, flow-control and resource-leak issues that do not appear on a perfect local loopback.

Approach and Components (beyond the existing `TestTransport` / `FaultInjectingTransport` tests)

- Proxy-based user-space fault injection (medium priority):
  - Integrate Toxiproxy or a small custom proxy harness for tests that exercise full binaries without requiring NET_ADMIN privileges.
  - Use the proxy to inject latency, connection cuts, truncation and partial writes to validate end-to-end behaviors.

- Kernel-level integration using `tc netem` and network namespaces (lower priority):
  - Create e2e tests that run client & server in separate Linux network namespaces connected by a veth pair with `tc netem` rules applied.
  - Simulate real TCP behaviours (retransmits, delayed ACKs, segment coalescing, zero-window events) that only the kernel stack exhibits.
  - These tests are heavier and run in nightly or scheduled CI only; they are optional for PRs because they require privileged runners and can be flaky.

Test Design & Best Practices

- Start with syscall error injection tests (EINTR, EAGAIN, EPIPE, ECONNRESET) and partial I/O.
- Prefer deterministic `TestTransport` unit tests for core protocol logic: easier to reproduce and debug.
- For integration tests, capture detailed artifacts on failures: pcap, logs, and deterministic seeds used by the test harness.
- Expose test-time configuration hooks (shorter timeouts, deterministic timers) so tests run quickly and reliably.

CI Policy: deterministic and fault-injection tests run on PRs; proxy-based and `tc netem` suites run nightly, non-blocking for PRs.

Milestones

1. Add HTTP/2 direct protocol tests using `TestTransport` + `processInput()` for frame-level fault injection.
2. Add simple Toxiproxy-based harness and a handful of end-to-end tests (connection cut, high latency, truncated responses).
3. Add `ip netns` + `tc netem` scripts in `tests/e2e/` and integrate nightly CI job.
4. Collect failure artifacts (pcap + logs) and add tooling to reproduce failing scenarios locally with the same netem parameters.

Known Limitations

- **Read-EAGAIN incompatible with EPOLLET in integration tests**: When `FaultInjectingTransport` returns `{0, ReadReady}` (simulated EAGAIN), the server breaks to wait for a new EPOLLIN edge. But with EPOLLET, the socket already has data so no edge fires - causing a hang. Workaround: EAGAIN on reads is tested only at unit level; integration tests use partial reads (which work because `EPOLL_CTL_ADD` with existing data triggers an initial event).

Acceptance Criteria

- Protocol correctness under simulated faults: no state corruption, proper error propagation, and graceful teardown.
- No resource leaks (sockets, memory) in faulted runs.
- Deterministic unit tests reproduce failures locally with a seed and do not rely on privileged resources.

## Test suite reduction and speed

Status: a first pass was done on 2026-11-08 (6024 -> 5888 tests, about 1,600 lines less, full `ctest -j12` from 11.4 s to 9.3 s, no covered line lost). Nothing below is implemented yet. Numbers are from that date, in a Debug build under `ctest -j12`. The slowest binaries are then `http-core_test` (9.2 s), `http-client-core_test` (7.3 s) and `http-client-tls-e2e_test` (5.6 s).

Goals

- Every test binary well under 10 s in Debug.
- Each behaviour tested once, at the layer where it is cheapest, plus one integration scenario proving the wiring.
- No loss of coverage, nor of asserted behaviour.

Rules for a reduction pass

- Prefer merging into a table over deleting: tests going through the same code often assert different values (parsers, formatters, validators). Delete only literal repeats and tests whose assertions are a subset of another test.
- Compare line and branch coverage per file before and after (lcov export of the coverage build): totals can hide a line lost here and gained there. The usual loss is an rvalue overload, when a merge keeps only the lvalue call.
- A few branches differing between two runs is noise (timing-dependent paths, `compare_exchange_weak`): run twice before investigating.
- The docs link to tests by name: search `docs/` for each removed name.
- Merged tests on a shared server keep its config and route leaks: run the whole binary, not only the merged test.
- Do not shorten the timings of tests tuned for loaded CI runners without a deterministic replacement.

### Finding candidates

- **Near-duplicate bodies**: extract every test body, replace the literals by placeholders and cluster by Jaccard similarity of 4-token shingles (threshold 0.8). Result: 398 clusters, 1336 tests, 16,373 lines.
- **Per-test edge coverage**: a throwaway tree built with `-O0 -g1 -fsanitize-coverage=edge,inline-bool-flag,pc-table` and a gtest listener linked in each test executable, resetting the flags in `OnTestStart` and dumping them in `OnTestEnd`, gives the library edges reached by each test (about 5 minutes in RAM). Edges are needed because source-based coverage regions can be differences of counters, which makes per-test subsets wrong. Pitfalls: the allowlist needs `fun:*` on top of its `src:` lines; `-z defs` must be dropped so that the shared libraries can reference the callbacks defined by the executable; a module is identified with `dladdr` on its flags, not on its first PC (an interposed comdat function points to another module); only the edges of library sources count (symbolize the function entries).
- 3904 of 6023 tests had no edge of their own at the first measurement. That only ranks candidates: combine it with the clusters and read the assertions.

### Remaining duplicated tests

| Test file | Lines in clusters | What to merge |
| --- | --- | --- |
| `http2-protocol-handler_test.cpp` | 1633 | `StreamingHandler*` single-request variants, file payloads (`LargeFilePayloadFlowControlledSending`, `FilePayloadExactlyFillsInitialWindow`), CORS (`*WithCorsApplied`, `*MiddlewareShortCircuitWithCors`, `CorsPreflight*`), `Options*` / `TraceReturns405InHttp2`, `PerRouteMaxHeaderBytes*` and `PerRouteMaxBodyBytes*` pairs, `RequestTrailers*Rejected`, `*Http2Disable*Returns404`. Keep the `AsyncHandler*DeferWork*` tests, which have many unique edges. |
| `tests/http-core_test.cpp` | 664 | `HttpResponseWriterFailures` (`FileAfterWriteBody`, `WriteBodyWithFileActive`, `ContentLength*`, `ModifyAfterHeadersSent`), `HttpStreaming.SendFile*`, `HttpCorsDetailed.Preflight*`, `HttpCorsIntegration.Streaming*`, `HttpStreamingHeadContentLength.*`, `HttpHeaderTimeout.Emits408*`, `HttpRateLimit.*` pairs. |
| `router_test.cpp` | 576 | `CatchAll*`, `Overwrite*`, `AllowedMethods*`, `ConstraintUnconstrainedFallback*`, patterns that must throw (`*Throws`, `*Disallowed`). |
| `http-client-core_test.cpp` | 537 | `FollowsRedirect`, `FollowsMovedPermanently`, `FollowsTemporaryRedirect`, `FollowsPermanentRedirect`, `StatusRetry*`, `HttpClientTrailerE2ETest.*`. |
| `upgrade-handler_test.cpp` | 523 | `ValidateWebSocketUpgrade_*`: a table for extensions and deflate offers, one for subprotocols, one for rejected requests. |
| `hpack_test.cpp` | 505 | The 21 decoding scenarios around invalid fields (`Rejects*`, `DuplicateIndexedHeaderForbidden`...), `EncodeNeverIndexed_*` / `EncodeWithoutIndexing_*`. |
| `http2-connection_test.cpp` | 407 | The frame validation family (`*IsFrameSizeError`, `*IsProtocolError`, `*IsFlowControlError`): a table of frame bytes and expected error. `PriorityChain*`. |
| `http-request-view_test.cpp` | 403 | `MergeConsecutiveHeaders*` / `MergeNonConsecutiveHeaders*`, the requests rejected on parsing, `MakeResponse*`. |
| `static-file-handler_test.cpp` | 390 | `MultiRange*` (17 tests), `Conditional*`. |
| `multipart-form-data_test.cpp` | 371 | The 24 rejected or accepted bodies: a table of body and expected outcome. |
| `server-config-loader_test.cpp` | 362 | `RouterCorsPolicy*`, `*SaveConfig{Yaml,Json,Yml}`, `Duration*RoundTrip`, field mapping tests with identical edges. |
| `tls-ticket-key-store_test.cpp` | 345 | `ProcessTicketFailsWhen*`, `LoadStaticKeys*`. |
| `route-constraint_test.cpp` | 341 | The 33 regex and quantifier cases, the 9 malformed patterns. |
| `tests/http-compression_test.cpp` | 335 | `DirectCompression_*`, negotiation results (`GzipChosenWhenHigherPreference`, `QValuesAffectSelection`...). |
| `websocket-handler_test.cpp` | 310 | `Utf8*`, reserved opcodes, `*MessageTooLarge`, masked pings. |
| `http-request_test.cpp` (client) | 309 | `ResolveRedirect*`: a table of base, location and result. |
| `http-response_test.cpp` | 309 | `SendFilePayloadOffsetLength*`, `SendFileHead*`, `Body_OnMode_Bypasses*`, `Body_BelowMinBytes_NoDirectCompression` / `Body_OffMode_NoCompression`. |
| `websocket-frame_test.cpp` | 307 | `Parse*` rejections, `HeaderSize*`. |
| `http1-response-parser_test.cpp` | 300 | Rejected responses, `Connection` token lists. |
| `tests/http2-core_test.cpp` | 294 | The 13 `Http2Streaming.*` single-handler tests. |
| `tests/http-routing_test.cpp` | 279 | `AsyncHandler*WithBodyNotReady`, `AsyncReadBodyBeforeBody*`. |
| `tests/http-additional_test.cpp` | 275 | `HandlerCanEmit*`, `EpollCtlMod*`, `EpollRdhup*` / `EpollHup*`. |
| `tests/http-errors_test.cpp` | 263 | Tables already, whose drivers can be shared. |
| `timestring_test.cpp` | 244 | Rejected dates and the formatting cases (identical edges, the values matter). |
| `websocket-deflate_test.cpp` | 227 | `ParseDeflateOffer_*`, `BuildDeflateResponse_*`. |

Other kinds of duplication:

- **Across files** (19 clusters, 766 lines):
  - `brotli-`, `zstd-` and `zlib-encoder-decoder_test.cpp` repeat the same scenarios per codec: typed tests.
  - `http-client-core_test.cpp` and `http-client-http2-e2e_test.cpp`: scenarios parameterized on the protocol. `http-client-integration_test.cpp` also repeats basic ones.
  - Header key and value validation in `http-client-config_test.cpp` and `http-server-config_test.cpp`.
  - "Default config is valid" tests, repeated per config object.
- **Across layers**: behaviours tested in full in both a unit and an integration test can keep one integration scenario: Range and conditional requests (`HttpRangeStatic`, `HttpRangeMulti` and `static-file-handler_test.cpp`), CORS (`HttpCorsDetailed`, `cors-policy_test.cpp` and the HTTP/2 handler tests), HTTPS redirect, request parsing errors.
- **HTTP/1.1 and HTTP/2 pairs**: behaviours tested separately in `http-core_test` and `http2-core_test` can move to the scenarios of `tests/http-protocol-parity_test.cpp`, which run on both protocols.
- **Identical edges, values that matter**: `websocket-compress_test.cpp` (groups of 13 and 9 tests), `tchars_test.cpp`, `base64_test.cpp`, `mergeable-headers_test.cpp`, `file-sendfile_test.cpp`: tables, without dropping cases.

### Repeated boilerplate

- `http2-protocol-handler_test.cpp`: 168 `Http2ProtocolLoopback` setups, 222 `pumpClientToServer()` calls, 38 identical default routes and 22 hand-built pseudo-header blocks. A helper that sends a list of pseudo-headers and pumps both ways would shorten most tests.
- `RequestOptions` + `requestOrThrow` + `parseResponseOrThrow` (74 uses in `http-core_test`, 20 in `http-routing_test`): one helper returning the parsed response.
- `ClientConnection` + `sendAll` + `recvUntilClosed` (86 connections in `http-core_test`, 77 in `http-additional_test`, 33 in `http-errors_test`): `test::sendAndCollect` when the socket is not used afterwards. The first pass only rewrote the 30 exact matches.
- `StaticFileHandlerTest`: the sequence of `buildReqWithHeaders`, `setHead` and handler call, in each test.
- One convention for table tests: `SCOPED_TRACE` loops are lighter, `TEST_P` (used in 7 files only) can filter and report each case.

### Slow tests

Dead waits, safe to remove:

- `test::recvWithTimeout` does not stop at an EOF received before a complete head: it spins until its timeout. It should return on `recv() == 0`.
- 81 calls use its default 500 ms timeout without an expected size (20 in `http-core_test`, 18 in `http-errors_test`, 11 in `websocket-integration_test`): each one not reaching a complete response waits the whole timeout.
- `recvWithTimeout(fd, 1000ms)` before a `WaitForPeerClose`, whatever is received: `HttpResponseDispatchErrors.QueueDataTransportError`, `HttpResponseDispatchErrors.FlushOutboundTransportError`, `Http1WriterTransport.WriteBodyReturnsFalseAfterTransportError` (1 s each). `RecvUntilEnd` gives the data and the end in one call. Same for `HttpTlsNegative.PlainHttpToTlsPortRejected` (0.5 s).
- Private servers on the default `pollInterval` of 500 ms, whose stop waits for a whole poll: six tests of `http-server-lifecycle_test` sit at exactly 0.5 s, `HttpServerMove.MoveAfterCompressingKeepsCompressing` (three servers) at 1.5 s, and probably `HttpTlsMoveAlpn.*`. Use `withPollInterval` unless the poll interval is what is tested.
- Plateaus not explained yet: 0.25 s (`HttpRouting.AsyncHandlerThrowsWithBodyNotReady`, `HttpRouting.AsyncHandlerNonStdExceptionWithBodyNotReady`, `HttpClientTlsE2ETest.GetOverTlsWithoutVerification`, `HttpClientTlsErrorTest.PartialClientCertificateFileConfigurationIsIgnored`, `HttpClientMtlsTest.RejectsClientWithoutCert`, `Http2ConnectTest.WildcardAllowlistAllowsTarget`) and 0.5 s (`Http2ConnectTest.DnsFailureReturns502`).

Waits for a negative outcome:

- `TunnelResolverTest.CancelDropsQueuedResolutions` (1.5 s) waits its whole 1500 ms bound for resolutions that must never complete: wait for the last resolution in progress instead. To check as well: `TunnelResolverTest.DestructionDoesNotWaitForResolutionsInProgress` (1.25 s).

Waits for real time, needing an injectable clock or a sub-second interval:

- `HttpTlsHandshakeCallback.RefillsRateLimitAfterInterval` (1.8 s, whole-second token bucket).
- `TlsTicketKeyStoreTest.RotateExceedsMaxKeysPopsBack` (1.1 s) and `HttpTlsSessionTickets.AutoRotationRefreshesPrimaryKeyAndRejectsUnknown` (1 s), rotation intervals in whole seconds.
- `HttpDate.ChangesAcrossSecondBoundary` (up to 1 s).

Real work, to reduce:

- `TlsContextTest.EveryAllocationFailureDuringConstructionThrowsCleanly` (4.1 s, the slowest test): one context construction per allocation, with a smaller certificate set for example.
- `HttpCodecCompression.TryCompressResponseStressWithDifferentScenarios` (1.7 s): seven nested dimensions, pairwise combinations would do.
- `WebSocketUtf8.EveryLeadWithBoundaryContinuations`, `RandomMutations` and `EveryBytePair` (2.9 s together): exhaustive, a smaller sample in Debug.
- Payload sizes of `ProtocolParityFastBrotli.LargeIncompressibleBodiesAreCompressedWhole` (0.8 s twice), `HttpClientSinkE2ETest.ContentCodedBodiesAreDecodedAsTheyArrive` (0.7 s) and `HttpClientCompressionE2E.LargeIncompressibleDirectlyCompressedRequestBody` (0.6 s).

Tuned to real timing, to leave alone without deterministic hooks: `ZerocopyMode.*`, `HttpBackpressure.*`, `HttpKeepAlive.*` (most of the remaining time of `http-core_test`), `HttpConnectDefaultConfig.SlowResolutionDoesNotBlockTheEventLoop`, `WebSocketTimeouts.PongsKeepIdleConnectionOpen`, `HttpClientErrorE2ETest.InactivityTimeoutLetsASteadySlowResponseComplete`, `Http2Core.PingKeepAliveClosesTheConnectionOfADeadPeer`.

### Test infrastructure

- **Commit the tools**: the near-duplicate clustering and the per-test edge coverage, in `scripts/`, so that each pass does not have to rebuild them.
- **Duration budget in CI**: from the gtest JSON output (`GTEST_OUTPUT`), warn when a binary exceeds 10 s in Debug.
- **Test isolation**: the binaries sharing one server leak config and routes between tests (71 config updates on the shared server of `http-core_test` alone), so `--gtest_shuffle` and `--gtest_repeat` fail spuriously. `ScopedConfigUpdate`, or a reset of config and routes in a fixture `SetUp`, would remove the dependency on test order.
- **Sharding**: once isolated, the largest binaries can run in several processes (`GTEST_TOTAL_SHARDS`): the longest binary sets the wall time under `ctest -j`.
- **Split the largest files by theme**: `http2-protocol-handler_test.cpp` (7047 lines), `tests/http-core_test.cpp` (6559), `http-response_test.cpp` (5914), `http-client-core_test.cpp` (5221), `http2-connection_test.cpp` (4521). Each file is one translation unit recompiled at each edit.
