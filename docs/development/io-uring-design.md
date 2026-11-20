# io_uring backend: design study (draft)

Draft of 2026-11-18, against `main` at `1242e218`. It decides whether and how aeronet should get an `io_uring` backend on Linux; nothing here is implemented.

## Recommendation

**Do it in stages, and expect to stop after the first one.** Only the hybrid seam is worth building: the ring becomes the blocking wait of the event loop and the input path of plain connections, while output, TLS, files, timers and `HttpClient` keep the readiness code, and epoll stays the default and the fallback. Its measured ceiling on the development machine is +3.2% to +3.6% on HTTP/1.1 keep-alive and +7% on connection churn, for an estimated 1,500 to 2,000 lines, a second Linux input path to keep correct across kernels, and buffer-lifetime bugs that AddressSanitizer does not see (verified below). That is below the stop criterion of [section 11](#11-staged-plan-and-stop-criterion), so stage 0 is a measurement, not a backend: run the same probe on GitHub-hosted runners, and start stage 1 only if the ceiling there reaches **+8% requests/s on HTTP/1.1 plain keep-alive at 100 connections, or +12% connections/s on churn**. Whatever the outcome: take the cheaper epoll-side wins first (no `epoll_ctl(DEL)` before `close()`, one read per timer tick), so that the ring is compared with the best epoll loop; and keep io_uring off by default at both levels (CMake option and runtime field), falling back to epoll with one log line when the kernel, a sysctl or a seccomp profile refuses it. Asynchronous reads of cold files are a separate question with a stronger case, since a page-cache miss blocks the whole loop for 80 µs to 0.8 ms here; [section 13](#13-separate-track-asynchronous-reads-of-cold-files) treats it, and finds that a thread pool may serve it as well as a ring.

## Baseline and evidence

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

**Markers**: **[code]** read in the sources at `1242e218`; **[netstudy]** measured by the network cost study above (a local report, not in the repository); **[probe]** observed on kernel 7.0.0-43 by the 2026-11-10 study or by the probe of this document ([appendix B](#appendix-b-probes)); **[doc]** kernel or liburing documentation, not checked here, `(?)` marking the versions I am least sure of; **[assumed]** reasoning only.

### What depends on readiness today [code]

| File | Functions | Readiness assumption | In the hybrid |
| --- | --- | --- | --- |
| [event-loop.cpp](../../aeronet/sys/src/event-loop.cpp) | `EventLoop::add`, `mod`, `del`, `poll` | `epoll_ctl` / `epoll_wait`, `EventFd` laid out as `epoll_event`, edge-triggered | Unchanged, nested in the ring |
| [timer-fd.cpp](../../aeronet/sys/src/timer-fd.cpp), [event-fd.cpp](../../aeronet/sys/src/event-fd.cpp) | `TimerFd::drain`, `EventFd::read` / `send` | Maintenance tick and cross-thread wakeup are readable fds | Unchanged, in epoll |
| [transport.hpp](../../aeronet/sys/include/aeronet/transport.hpp) | `PlainTransport::read`, `write`, `Transport::hasPendingReadData` | `ReadReady` / `WriteReady` mean "wait for an event"; data buffered above the socket is polled explicitly | Ring connections read the completion's bytes first |
| [zerocopy.cpp](../../aeronet/sys/src/zerocopy.cpp) | `ZeroCopyState::pollCompletions` | Error queue read when `EPOLLERR` fires | In epoll while sends are in flight |
| [sendfile.cpp](../../aeronet/sys/src/sendfile.cpp), [tcp-connector.cpp](../../aeronet/sys/src/tcp-connector.cpp) | `Sendfile`, `ConnectTCP`, `WaitForConnectCompletion` | `EAGAIN` waits for writable; connect completes with `POLLOUT` + `SO_ERROR` | Unchanged |
| [tls-transport.cpp](../../aeronet/tls/src/tls-transport.cpp) | `TlsTransport::read`, `hasPendingReadData`, `trackShortSocketReads`, `handshake` | Socket BIO read until drained; records read ahead raise no edge; `WANT_READ` / `WANT_WRITE` become readiness | TLS stays on epoll |
| [single-http-server.cpp](../../aeronet/server/src/single-http-server.cpp) | `eventLoop`, `handleConnectionEvent` | Blocks in `poll()` (`poll(false)` while inputs are deferred), dispatch by fd, an unknown fd is a stale event | Blocks in the ring; recv completions get their own dispatch |
| | `acceptNewConnections`, `handleReadableClient`, `resumeDeferredInputs`, `handleWritableClient` | `add(EventIn \| EventRdHup \| EventEt)` at accept; a short read drained the socket; the fairness budget defers the rest | Plain connections attach to the ring; "drained" arms a recv |
| [connection-io.cpp](../../aeronet/server/src/connection-io.cpp) | `enableWritableInterest`, `disableWritableInterest`, `flushOutbound`, `flushFilePayload` | `EPOLL_CTL_MOD` toggles `EventOut`; writes until `EAGAIN`, retried once after enabling the interest (edge race) | Ring connections: `ADD` / `DEL` with `EventOut` only |
| | `deferInput`, `resumeInputIfOutputDrained`, `startLingeringClose`, `discardLingeringInput` | Input left unread raises no new edge; a lingering close reads until EOF | Unchanged: no recv is armed meanwhile; ring recv until EOF |
| | `releaseConnection` | `epoll_ctl(DEL)` then `close()`, which purges pending events | Cancel the armed recv, bump the generation |
| [connection-storage.hpp](../../aeronet/server/include/aeronet/internal/connection-storage.hpp) | `ConnectionStorage::iterator(fd)` | Indexed by fd: on Linux "the poller never returns a closed fd" | Generation check first |
| [http-server-lifecycle.cpp](../../aeronet/server/src/http-server-lifecycle.cpp) | `initListener`, `runUntilStarted` | `EventLoop` built on the controller thread for `MultiHttpServer` workers, polled on the loop thread | Ring created and closed on the loop thread |
| [tunnel-manager.cpp](../../aeronet/server/src/tunnel-manager.cpp) | `registerUpstream`, `handleWritable`, `relayInput`, `readInput` | Connect completion by `EPOLLOUT`; stop reading by `MOD` to `EventOut` only | Tunnel endpoints move to epoll |
| [connection-maintenance.cpp](../../aeronet/server/src/connection-maintenance.cpp) | `sweepIdleConnections` | Retries writes whose writable edge was missed | Unchanged |
| [http-client.cpp](../../aeronet/client/src/http-client.cpp) | `HttpClient::armLoop`, `waitEvents` | Private `EventLoop`, one fd at a time | Unchanged |

## 1. Seams compared

| Seam | Expected gain, and why | Size | Risk | Portability |
| --- | --- | --- | --- | --- |
| **(a)** `EventLoop` shim: `add` / `mod` / `del` = multishot `POLL_ADD`, update, remove; `poll()` = `io_uring_enter` | Keep-alive about 0: no `epoll_ctl` to batch in steady state, and the wait is one syscall either way. Churn 2.5% at most (`ADD` + `DEL`: 317 ns of 12.7 µs), most of which skipping `DEL` gives on epoll (+0.6% to +1.1% measured) | Small, `aeronet/sys` only: about 600 lines | Medium: multishot poll is not epoll (a completion per wakeup, ended without `IORING_CQE_F_MORE` on overflow, no `EPOLLEXCLUSIVE`), and a poll request holds a file reference, so `close()` no longer purges events: section 2's generations are needed anyway. The old `feature/io_uring` branch found this emulation the fragile part | None |
| **(b)** Completion-based transport: multishot recv into provided buffer rings, ring sends, registered files, multishot accept | Below (c) on aeronet's request/response pattern: multishot recv +0.4% to +1.5%, ring sends -2.8% to -4.0%. May do better where syscalls cost more (VMs with mitigations): stage 0 measures it | Very large: asynchronous output (output queue, streaming writers, HTTP/2 `onOutputWritten`, OpenSSL through memory BIOs, `sendfile` through pipes, zerocopy), connections outliving their close. The old branch, a full proactor, added 2,480 lines over 42 files | High: send ordering, buffers held until completion, multishot recv ignoring backpressure | High: two output models in the protocol layers, unless every backend becomes completion-based (the IOCP roadmap item) |
| **(c)** Hybrid: the ring is the wait and the input of plain connections (single-shot recv with buffer selection, armed once the socket is drained), optionally multishot accept; synchronous output; epoll nested in the ring | Ceiling +3.2% to +3.6% keep-alive, +7% churn, about -3% at 8 connections; about 0 for multiplexed h2c and for TLS | Medium: 1,500 to 2,000 lines with tests (ring wrapper 300, buffer pool 150, slots and dispatch 350, about 8 guarded server touch points 250, tests 700); protocol layers and `ConnectionState` unchanged | Medium: armed recvs outlive `close()`, stale completions (verified), close timing | None: Linux only, behind the option |

**Choice: (c).** (a) brings nothing for its risk; (b) loses on the measured pattern and makes the protocol layers depend on the backend. In (c) the ring holds one multishot `POLL_ADD` on the epoll fd [probe: one completion per new readiness, none while idle]; when it fires, `EventLoop::poll(false)` collects the readiness events as today: wakeup eventfd, timerfd, writable waits after `EAGAIN`, zerocopy error queues, TLS connections, tunnels. These cold fds cost one extra syscall only in the iterations where one fires. The ring code is a dedicated server component (`internal::RingInput`, bound in `ServerContext::bindComponents()`): `SingleHttpServer` and `ConnectionIo` only gain guarded calls.

## 2. Buffer ownership and lifetime

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

## 3. TLS

Today [code], `TlsTransport` binds its `SSL` object to the socket (`SSL_set_fd()` in `TlsAcceptor`) with read-ahead. `read()` decrypts until the socket is drained, detected by a read-BIO callback (`trackShortSocketReads()`); `hasPendingReadData()` reports records read ahead; writes go through `SSL_write_ex()` record by record, or straight to the socket once kTLS send is on (now off by default).

| Option | What it does to `TlsTransport` | Gain | Verdict |
| --- | --- | --- | --- |
| T1: TLS stays on epoll | Nothing | None | Stages 1 and 2 |
| T2: memory BIO (or a custom `BIO_METHOD`) between ring and OpenSSL | `SSL_set_bio()` instead of `SSL_set_fd()`; handshake flights and alerts flushed from the write BIO explicitly; `WANT_READ` means "no ciphertext buffered"; `trackShortSocketReads()` replaced by the completion flags; no kTLS (OpenSSL enables it through its socket BIO); `releaseBuffers()` covers the BIO buffers; one more copy of the ciphertext, avoided by a custom BIO reading the provided buffer | The read syscall, 1 of 2 per request since #968: about 0.15 µs of 6.5 µs, 2% to 3%. The larger write-side gain (one `send()` for several records, against 65 writes per MiB, about 8% at 1 MiB) needs no ring: a write memory BIO on epoll gives it | Stage 3, once the write memory BIO is measured on epoll, so the ring is not credited with its gain |
| T3: kTLS send and receive, ring operations as on a plain socket | kTLS receive needed (OpenSSL 3.0: TLS 1.2 only, 3.2+: TLS 1.3); a non-data record (alert, KeyUpdate) fails a plain recv with `EIO` [doc], hence `RECVMSG` with a control buffer | Read and write syscalls | Rejected: kTLS send measured 5% to 54% slower than user-space TLS for in-memory bodies on the 3 CI CPU types |

## 4. Timers

Today [code], one periodic timerfd per loop, at the smallest of the enabled timeouts (keep-alive, header read, body read, TLS handshake) and `pollInterval` (`updateMaintenanceTimer()`), drives the maintenance: the keep-alive deadline heap (`KeepAliveDeadlineQueue`, one O(log n) update per event), header read, body read, request and handshake timeouts as timestamps swept only while some connection needs it, HTTP/2 stream deadlines. The poll timeout never fires on Linux, the timerfd coming first [netstudy].

- **Linked timeouts** (`LINK_TIMEOUT`, 5.5) per recv: rejected. Per request they add an SQE, a completion and an hrtimer armed and cancelled, where the heap costs one update per event; and they bound one operation, while aeronet's timeouts span several: a head sent a byte per second completes a recv each second, so a per-recv timeout never fires (the slowloris case `headerReadTimeout` exists for); keep-alive idleness is also refreshed by output progress (`lastActivity` in `flushOutbound()`); HTTP/2 and WebSocket timeouts have no pending recv to link to.
- **Timeout operations**: the wait's own timeout (`IORING_ENTER_EXT_ARG`, 5.11) replaces `epoll_wait`'s. A multishot `TIMEOUT` (6.4 (?)) could replace the timerfd and its 2 reads per tick: not worth a change, the timerfd stays in the nested epoll.
- **Decision**: the deadline queue, the header read timer and the maintenance tick do not change and do not know the backend.

## 5. Files and zero copy

Today [code], files up to `inlineFileThresholdBytes` (128 KiB by default) are inlined into the response; larger ones follow the head with `sendfile()` in 2 MiB chunks (`ConnectionState::transportFile()`), with writable interest and one immediate retry on `EAGAIN`; user-space TLS reads 128 KiB chunks with `pread()`. Large in-memory plain responses use `MSG_ZEROCOPY`: buffers retained until completion (`ZerocopyBuffers`), completions read from the error queue on `EPOLLERR`, drain closes waiting for them, otherwise an abortive close and a one second grace period.

- **sendfile and splice**: io_uring has no sendfile operation. Its equivalent, two `SPLICE` operations (5.7) per chunk through a pipe, is bounded by the pipe capacity (64 KiB by default), so more operations per MiB than one `sendfile()` of 2 MiB, which uses the same splice machinery in the kernel [doc]. `sendfile()` stays.
- **The real file-side gain** would be not blocking the loop on page-cache misses, which `pread()` and `sendfile()` of a cold file do today [code]. That is a separate track, independent of the network backend: [section 13](#13-separate-track-asynchronous-reads-of-cold-files).
- **`SEND_ZC` (6.0) against the zerocopy module**: the send result comes as a completion, then a notification (`IORING_CQE_F_NOTIF`) once the buffer is released: no error queue, no `SO_ZEROCOPY`, and with registered buffers no page pinning per send [doc]. It fits the existing retention machinery, but its result is asynchronous: zerocopy sends would become the first asynchronous writes, or be submitted and reaped in one `io_uring_enter`. Gains need a NIC: on loopback `MSG_ZEROCOPY` measured -28% at 256 KiB and -37% at 1 MiB [netstudy]. Stage 3, between two hosts.
- **Zerocopy in the hybrid**: a ring connection with zerocopy sends in flight joins the nested epoll for `EPOLLERR` until they complete, like a writable wait.

## 6. HTTP/2

Today [code], `processSpecialProtocolHandler()` processes every complete frame of the input buffer, one gathered write sends the pending fragments (`getPendingOutputFragments()`), and `onOutputWritten(written)` releases what was written, at once.

- **Many small frames** arrive in one completion as in one `recv()` today, and leave in one gathered write per flush. Per-frame submissions (an SQE and a completion each) are never made.
- **Gain**: multiplexed h2c makes 0.1 recv per request [netstudy], so the ring saves 0.1 syscall entry per request, a few nanoseconds. One stream per connection behaves like HTTP/1.1.
- **Flow control**: HTTP/2 flow control is in WINDOW_UPDATE frames, and the server's backpressure (`pendingOutputSize()` against `maxOutboundBufferBytes`, `stopReadingIfOutputBlocked()`, `relayPausedByBacklog`) works by not reading. A single-shot recv keeps that exact; a multishot recv would keep filling buffers: rejected.
- **Ordering**: synchronous output keeps frames in queue order, partial writes known at once, and the `onOutputWritten()` contract. Ring sends would need at most one send in flight per connection (two sends on a socket armed for poll may complete out of order unless linked [doc]), RST_STREAM, GOAWAY and WINDOW_UPDATE queued behind it, and the DATA buffers of a reset stream kept while an SQE points to them: an ownership change inside the HTTP/2 layer, which the hybrid avoids.

## 7. Threads

- **One ring per event-loop thread**: per `SingleHttpServer`, so one per `MultiHttpServer` worker, each with its own `SO_REUSEPORT` listener [code]. Nothing is shared (no `MSG_RING`, no `ATTACH_WQ`); connections keep the kernel's hash distribution, imbalanced at 2 connections per thread or fewer [netstudy].
- **Created on the loop thread**: `SINGLE_ISSUER` (6.0) ties the ring to its creating thread (or enabling thread, with `R_DISABLED`) [doc]. `MultiHttpServer` builds its workers' event loops on the controller thread (`ensureNextServersBuilt()`, `initListener()`, then copies) [code], so the ring is created at the start of `runUntilStarted()` and closed at its end, once per run: moves and restarts, done while stopped, never see a live ring.
- **`DEFER_TASKRUN`** (6.1): completions are processed only in `io_uring_enter` with `GETEVENTS` [doc], so task work never interrupts a handler and completions batch at the wait. They are not visible by peeking at the CQ, so `poll(false)` (taken while inputs are deferred) costs one `io_uring_enter`, like today's `epoll_wait(0)` (46 ns).
- **Cross-thread wakeups** (`wakeupFd.send()` for updates, `stop()`, async resumes) keep the eventfd in the nested epoll, read with libc `read()`: no new synchronization, and ThreadSanitizer keeps the edge it derives from that read.
- **SQPOLL: not wanted.** A kernel thread per ring spins on the submission queue, then sleeps after `sq_thread_idle` and needs a syscall to wake: one more busy core per loop, where aeronet maps loops 1:1 to cores, to save a syscall the hybrid already amortizes over a batch. It cannot be combined with `DEFER_TASKRUN` [doc], its CPU is not accounted to the loop thread (pinning and quotas get harder), and some policies restrict it (SELinux `io_uring { sqpoll }`).
- **io-wq**: recv with `POLL_FIRST` and accept are poll-driven, never handed to io-wq [doc]; cap the workers anyway (`IORING_REGISTER_IOWQ_MAX_WORKERS`, 5.15).

## 8. Detection, fallback and build

**Minimum kernel 6.1** (`DEFER_TASKRUN`), which brings everything else used: provided buffer rings, multishot accept, `POLL_FIRST`, `SOCK_NONEMPTY` (5.19), `SINGLE_ISSUER` (6.0). Ubuntu 22.04's 5.15 and RHEL 9's 5.14 fall back to epoll.

At the start of each run, on the loop thread, when the runtime field asks for the ring:

1. `io_uring_setup()` with `SINGLE_ISSUER | DEFER_TASKRUN | SUBMIT_ALL | CQSIZE | R_DISABLED`. Its error tells why: `ENOSYS` (no io_uring in the kernel or sandbox), `EPERM` (`kernel.io_uring_disabled` = 2, or 1 outside `kernel.io_uring_group`, or seccomp), `EINVAL` (kernel older than 6.1), `ENOMEM` / `EMFILE`.
2. `IORING_REGISTER_PROBE`: `RECV`, `ACCEPT`, `POLL_ADD`, `ASYNC_CANCEL`; features `NODROP`, `EXT_ARG`, `FAST_POLL`.
3. Buffer ring, restrictions ([section 9](#9-security-posture)), registered ring fd, io-wq cap; then enable the ring.

A failure closes the ring, logs once per server (info for `ENOSYS` and `EPERM`, warning otherwise), counts in the statistics, and the run uses epoll. No switch during a run. During the run, `io_uring_enter` failing with `EINTR`, or with `EBUSY` (completions kept by `NODROP` after a completion queue overflow, reaped first [doc]), is retried; any other failure stops the server, as a failing `epoll_wait` does. `CQSIZE` covers one recv per connection, so that an overflow stays exceptional.

**Runtime field**: `HttpServerConfig::ioBackend` (`IoBackend::Epoll` by default, `IoBackend::IoUring` when available), always present like `zerocopyMode`, ignored with one log line without the option, applied at the next run. A field, not only a build option: operators turn the ring off without rebuilding, and benchmarks A/B both backends in one binary, keeping code layout out of the comparison.

**Build**: `AERONET_ENABLE_IO_URING` (OFF, Linux only), on the raw UAPI (`<linux/io_uring.h>`, `syscall()`): about 300 lines (ring mmaps, SQE and CQE access with acquire / release on the indexes, buffer ring updates, 5 operations), no new dependency for FetchContent, Conan and vcpkg, the barrier code covered by unit tests. liburing (dual MIT / LGPL-2.1 [doc]) would bring ready helpers but a dependency and header/library version skew; it stays in the throwaway probes. Ubuntu 24.04's `linux-libc-dev` 6.8 defines every constant used, lacking only later ones (`IOU_PBUF_RING_INC`, `IORING_RECVSEND_BUNDLE`, `IORING_OP_EPOLL_WAIT`), checked in the installed header.

## 9. Security posture

- **Users who restrict io_uring see no change**: the default is epoll, and a refused setup falls back with one log line. Restrictions are common: Docker's default seccomp profile blocks the io_uring syscalls since Docker 25 [doc], containerd's too (version (?)), Kubernetes `RuntimeDefault` uses the runtime's; Google disabled io_uring on ChromeOS, Android apps and its production servers (2023, after most kernel exploits submitted to its bug bounty used it) [doc]; RHEL 9 reportedly ships it disabled (?); kernels from 6.6 have `kernel.io_uring_disabled`.
- **Users who enable it accept a larger kernel attack surface** for their whole container or host, not only aeronet: the io_uring syscalls allowed in seccomp, `kernel.io_uring_disabled` = 0 (or 1 with the group). The documentation must say so.
- **Visibility changes**: ring operations bypass per-syscall seccomp filters and syscall-based auditing (audit rules, sensors on syscall tracepoints): a connection accepted or read through the ring shows no `accept4` or `recvfrom`. The socket-layer LSM hooks still run [assumed: io_uring calls the same socket functions]. Open question 5.
- **Hardening**: the ring is created disabled (`R_DISABLED`), restricted to its operations (`IORING_REGISTER_RESTRICTIONS`, 5.10: `RECV`, `ACCEPT`, `POLL_ADD`, `POLL_REMOVE`, `ASYNC_CANCEL`), then enabled: a memory corruption in the process cannot turn it into an `openat` or `connect` primitive. No SQPOLL, no registered buffers (nothing charged to `RLIMIT_MEMLOCK`; ring memory goes to the memory cgroup since 5.12 [doc]).

## 10. Testing

- **AddressSanitizer does not see kernel writes** [probe, Q3]: with a provided buffer freed while in the ring, the kernel wrote 12 bytes into the freed memory unreported; only the later user-space read was reported, and that report crashed inside ASAN, the kernel having overwritten ASAN's record of the free. Buffer lifetime must hold by construction: one slab owned by the ring object, never freed while registered, section 2's teardown order, an assertion that nothing is in flight before the free. In ASAN builds, buffers given back to the ring are poisoned (`ASAN_POISON_MEMORY_REGION`) and unpoisoned on completion, so a user-space access to a buffer the kernel owns is reported.
- **ThreadSanitizer**: kernel accesses are invisible to it, and a single-issuer ring has no user-space concurrency. The wakeup keeps its libc `read()` (section 7), the ring indexes use `std::atomic_ref` acquire / release. The TSAN job gets a ring variant for the wakeup paths.
- **CI kernels**: the runners in use are `ubuntu-24.04` (x86_64, arm) and `ubuntu-26.04` (`ubuntu-latest`), whose job logs do not print the kernel: Azure kernels of 6.8 or later with io_uring enabled are likely [assumed]. Stage 0 prints `uname -r`, `kernel.io_uring_disabled` and the setup result. The Alpine job runs in a Docker container (API 1.54 on the runner), whose default seccomp profile should refuse io_uring [doc]: the fallback gets exercised there.
- **New job**: one Linux Debug job (ASAN, features on) runs the whole suite with the ring forced by a test hook (an environment variable read by the test support, like `ScopedTransportDecorator` for transports); existing jobs keep testing epoll.
- **Existing fault injection**:
    - `TestTransport` / `TestPipe` (no event loop): not concerned.
    - `FaultInjectingTransport` through `ScopedTransportDecorator` ([network-fault-injection_test.cpp](../../tests/network-fault-injection_test.cpp)): a decorated transport is not plain, so it stays on epoll and those tests keep testing what they test. Putting the ring input under the transport, for decorators to wrap, would grow `Transport` (asserted at 3 pointers) and put ring state into `aeronet/sys` transports: not proposed.
    - The libc interposers of `sys-test-support.hpp` (41 test files): output injections (`SetSendmsgActions` used 11 times, `SetSendfileActions` 5, `SetWriteActions` 4, and `AcceptInstallActions` 11, which the `accept4` interposer installs on the fd it accepts) still apply to ring connections in stage 1, whose accepts and output stay on libc; stage 2 must pin epoll for them and for the tests reading `g_last_accepted_fd`. Input ones (`SetEpollWaitActions` 12, `SetReadActions` 9, `FailAllEpollCtlMod` 6, `PushRecvAction` 5) do not see ring operations: those tests pin `IoBackend::Epoll`.
    - **Ring equivalent**: a weak callout around `io_uring_setup()` (like `AeronetSslSetFd()`) to fail the setup with `ENOSYS`, `EPERM` or `EINVAL`; and a completion hook at the dispatch boundary, driven by `FaultPolicy` where its fields apply: short counts, `-ENOBUFS`, `-ECANCELED`, `-ECONNRESET`, `SOCK_NONEMPTY` flipped, a completion delayed to the next iteration, a stale completion after close and fd reuse, a late one after a detach to epoll, CQ overflow. It shortens or delays data, never drops it, the limit [Testing](testing.md) notes for simulated `EAGAIN`.

## 11. Staged plan and stop criterion

Every A/B runs one binary with both `ioBackend` values, 5 alternating rounds, reporting mean, min and max, on the scenarios of `benchmarks/scripted-servers` plus churn (`Connection: close` through a Lua script). Hardware: the CI benchmark topology on GitHub-hosted runners (server on CPU 0, wrk on CPUs 1 and 2, several jobs to sample the runner CPU types) and the development machine.

| Stage | Content | Exit criteria |
| --- | --- | --- |
| **0. Measure** (no library code, a few days) | On a throwaway branch: the probe against today's best epoll loop (no `epoll_ctl(DEL)` before `close()`), churn paying `getpeername()` (multishot accept cannot return each peer address [doc]), a `workflow_dispatch` job printing kernel, sysctl, setup result and CPU model. On `main` meanwhile: skip `epoll_ctl(DEL)` before `close()` (+0.6% to +1.1% churn measured), `TimerFd::drain()` stopping after one read | Results per runner CPU type recorded here. **Go** only if, on the median CPU type, the ceiling reaches +8% requests/s on HTTP/1.1 plain keep-alive at 100 connections, or +12% connections/s on churn, with the 8-connection run not below -2%. Otherwise stop: results here, roadmap item closed as measured |
| **1. Ring wait and ring input** | Option, runtime field, `internal::RingInput`, nested epoll, fallback, restrictions, completion hook, CI job, docs (feature page, CMake option and configuration references, flag matrix) | Suite green on both backends under ASAN and TSAN; fallback tests for `ENOSYS`, `EPERM`, `EINVAL`; 100% coverage of new code; idle heap per connection unchanged (2,035 B at 10k idle plain connections, within 5%); at most 1.05 syscalls per request on HTTP/1.1 plain keep-alive at 100 connections; the stop criterion below |
| **2. Multishot accept** | Plain listeners; peer address by `getpeername()` or lazily; a bound on accepted but unserved connections, keeping the backlog pushback of `maxAcceptBatchSize`; cancellation before the listener closes | Churn +5% over stage 1, same guards; tests relying on the `accept4` interposer pinned to epoll |
| **3. Candidates**, each gated | Registered files (+1%); `SEND_ZC` between two hosts (CPU per GiB -10%); TLS memory BIO (+5% on HTTP/1.1 user-space TLS at 100 connections beyond the epoll write BIO); incremental buffers on 6.12+ (memory at 10k connections); `IORING_OP_EPOLL_WAIT` on 6.15+ (if it removes code) | As stated |

**Stop criterion.** Stage 1 is not merged unless, on the CI benchmark runners, the ring gives at least **+5% requests/s on HTTP/1.1 plain keep-alive `static` at 100 connections and +5% connections/s on churn at 100 connections** (mean of 5 alternating rounds, every ring round above every epoll round), and no guard fails: no other scripted scenario (HTTP/1.1, h2c, TLS; 10 and 100 connections) more than 1% below epoll, no p99 latency more than 5% above, the 1 and 8-connection runs at most 2% below, and behind a same-host nginx (keep-alive upstream, `/ping` then a 25 µs handler) server CPU per request no higher than with epoll. Below that, the branch stays unmerged, for reference.

## 12. Open questions

1. **The ceiling on CI-class hardware**: stage 0, on several runner CPU types (the kTLS check of 2026-11-18 met Xeon 6975P-C, EPYC 9V45 and EPYC 7763).
2. **Runner kernels and io_uring availability**, Alpine container included: the stage 0 job.
3. **The buffer pool at 10k active connections** (`-ENOBUFS` rate, memory without 6.12's incremental consumption): the 10k-connection scenario of the network cost study on the stage 1 build, deciding between a capped pool and requiring 6.12.
4. **Close timing**: may every close release its socket at the next wait (FIN, or RST of an abortive close, microseconds later), or must cancellations be submitted at once (one syscall per close, like `epoll_ctl(DEL)` today)? The lingering and zerocopy close tests on the stage 1 build, and a churn A/B of both.
5. **Security tooling**: do the LSM and BPF sensors operators run (like the one on the development machine) see ring socket operations, and do their policies flag io_uring? Not resolvable on a managed laptop: ask users, read vendor documentation.
6. **Defaults of RHEL 9, containerd, Podman, gVisor**: only the fallback depends on them; run the stage 1 fallback test in their images.
7. **IOCP**: a Windows IOCP backend sharing a completion-based core would change the cost of seam (b). Decide that roadmap item first.
8. **aeronet's bookkeeping against the probe's ceiling** (slot lookup, copy, arming): the stage 1 A/B with the network cost study's cycle and syscall counters.

## 13. Separate track: asynchronous reads of cold files

Sections 1 to 12 are about throughput. This one is about latency: while one connection waits for the disk, every connection of its loop waits too. It does not need the network backend, and io_uring is one of two ways to address it.

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
| Completion to the loop | An eventfd registered with `IORING_REGISTER_EVENTFD` (5.2) in the existing epoll, or the network ring of section 1 if it exists | The cross-thread path that already exists: a queue under a mutex, then `wakeupFd`, as async handler resumes do |
| Requirements | Async buffered reads (5.9 (?)), io_uring allowed (section 9) | None, every platform |
| Shared with | The ring wrapper of section 1 | The roadmap's thread pool for `deferWork()` |

A miss costs 80 µs to 0.8 ms of disk time, against which a few µs of thread handoff do not matter, and the pool needs no kernel feature, no relaxed seccomp profile, and serves `deferWork()` too. So: an asynchronous file read interface with a pool behind it first, and a ring backend only if it beats the pool on CPU per miss at high miss rates.

**Design.**

- **Interface**: an `internal::FileReader` component in `ServerContext`: a read is submitted with the connection's fd and generation, the file, an offset and a length; its completion (bytes or error) is dispatched on the loop thread.
- **Ownership**: while a read is in flight, its buffer and its `File` belong to the read, not to the connection. A close only orphans it (generation check, as in section 2), and its completion frees both. A pool worker reads through the fd number, so the `File` must stay open until the read completes: closed, its number could be reused by another open, and the worker would read another file. A ring holds its own file reference.
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

**Open questions of this track**: the latencies of real storage (cloud block storage, network file systems, container overlays), from F0 on the runners and from users; small compressed files (serve the first cold request synchronously, or compress in the output path); the pool size, and whether one pool serves file reads and `deferWork()` for the whole process; macOS and Windows, which have no `cachestat()` (`mincore()` needs a mapping): offload every read above a size, or none.

## Appendix A: kernel features

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
| `preadv2(RWF_NOWAIT)` | 4.14 (?) | Section 13 | [probe] `EAGAIN` on cold ranges |
| `IORING_REGISTER_EVENTFD`; `READ`, `OPENAT`, `STATX` | 5.2; 5.6 | Section 13, ring backend | [doc] |
| Async buffered reads | 5.9 (?) | Section 13, ring backend | [probe] no io-wq worker for cold reads on ext4 |
| `openat2(RESOLVE_CACHED)` | 5.12 | Section 13, F4 | [doc] |
| `cachestat()` | 6.5 | Section 13 | [probe] cold ranges reported, 0.1 to 0.5 µs |

## Appendix B: probes

Throwaway programs, not in the repository: liburing 2.5, kernel 7.0.0-43, `SINGLE_ISSUER | DEFER_TASKRUN`. The 2026-11-10 study measured the throughput table of the baseline and checked `TCP_NODELAY` inheritance from the listener, multishot poll on an epoll fd, and an armed recv after `close()`. This document's probe (about 200 lines) answered three questions:

| Question | Decides | Answer |
| --- | --- | --- |
| Q1. Can a completion name an fd number that was closed and given to a new connection? | Generations in `user_data` | Yes: the recv armed on the closed fd 6 completed after a new connection got fd 6, with the old peer's bytes |
| Q2. Do idle armed recvs with buffer selection pin provided buffers, and what does a recv get from an empty ring? | Pool sizing, overload path | Nothing is pinned: 64 idle recvs on 4 buffers, no completion; buffers taken on arrival; an empty ring completes the recv at once with `-ENOBUFS`, the data left in the socket (a direct `recv()` read it) |
| Q3. Does ASAN report the kernel writing into a provided buffer the program freed? | Whether sanitizers police buffer lifetime | No: the kernel write went unreported, only the later user-space read was, and that report crashed inside ASAN, whose record of the free the kernel had overwritten |

For section 13, a second probe (about 250 lines, a 64 MiB file on ext4, pages evicted with `POSIX_FADV_DONTNEED`):

| Question | Decides | Answer |
| --- | --- | --- |
| F1. How long does a page-cache miss block `pread()` and `sendfile()`? | Whether the track is worth it | 83 µs (16 KiB) to 778 µs (2 MiB), 440 µs for a `sendfile()` of 256 KiB, against 1 to 54 µs warm |
| F2. Do `preadv2(RWF_NOWAIT)` and `cachestat()` detect a miss without blocking, and at what cost? | Miss detection on the hot path | Both detected 21 of 21 cold ranges; `RWF_NOWAIT` returned `EAGAIN` after 16 µs and is the read itself on a hit; `cachestat()` took 0.1 to 0.5 µs |
| F3. Does an io_uring `READ` of a cold range return from the submission at once, and through io-wq? | Ring against thread pool | The submission took 16 µs (the loop still pays the I/O submission), completion 156 µs later; no io-wq worker thread appeared |
