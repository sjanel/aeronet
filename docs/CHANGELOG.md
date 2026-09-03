# CHANGELOG

All notable changes to aeronet are documented in this file.

## Unreleased

## Breaking changes

- **`HttpMessage` header and trailer names are now stored in lower-case**: this applies to `HttpRequest`, `HttpResponse`, `HttpResponseWriter`, received client responses, flat views, iterators and HTTP/1.x serialization. All APIs taking one header or trailer name (`hasHeader`, `headerValue*`, `headerAddLine`, `header`, `headerAppendValue`, `headerRemove*`, `hasTrailer`, `trailerValue*`, `trailerAddLine`) now take `LowerAsciiKey`, so uppercase literals fail to compile and dynamic keys must be normalized before wrapping. Preserving title-case field names on HTTP/1.x is no longer supported; lower-case field names are valid in HTTP/1.x and required by HTTP/2.
- **StaticFileConfig.maxMultipartRanges** zero value has no more special value meaning 'unlimited'. All queries containing ranges will be rejected with HTTP error 416. Also, the order of the configuration fields in `StaticFileConfig` slightly changed to decrease padding, beware if you used the aggregate constructor.
- **Http2Config.maxStreamsPerConnection** default value changed from `0` to `1000000`, and `0` does not have special **unlimited** meaning anymore.
- **DecompressionConfig.decoderChunkSize** is now a `uint32_t` and has been moved as the second field of `DecompressionConfig`.
- **DecompressionConfig.maxCompressedBytes** default value changed from 0 to `128MiB`, and `0` does not have special **unlimied** meaning anymore.
- **DecompressionConfig.maxExpansionRatio** default value changed from `0.0` to `1000.0`, and `0` does not have special **unlimited** meaning anymore.
- **DecompressionConfig.streamingDecompressionThresholdBytes**'s `0` value is not special anymore (previous meaning was "always use aggregated mode"). So a value of `0` will now use streaming decompression for all bodies.

## Bug Fixes

- **HTTP/1 request heads now reject a bare CR immediately**: the streaming parser distinguishes a CR at the current buffer boundary (incomplete, so it waits for LF) from a CR followed by any other byte (malformed, so it returns `400 Bad Request` without scanning for a later CRLF). This tightens malformed-message handling and avoids permissive recovery that can contribute to parser differentials.
- **HTTP/1.1 CONNECT no longer stalls on its first tunnel bytes**: bytes already buffered with the CONNECT head, or arriving while the `200` response completed, could be fed back into HTTP parsing and wait forever for another edge-triggered read event. The connection now switches the active read loop to tunnel forwarding immediately.
- **Static file ranges now follow RFC 9110 semantics**: byte-range units are case-insensitive, unsupported units andRange on HEAD are ignored, empty list elements do not count toward the configurable safety limit, multipart parts retain request order, and malformed or unsatisfiable ranges are distinguishe correctly.
- **HTTP client request deadlines now win over late socket readiness**: if a client thread was descheduled across its request deadline and resumed with a peer close or other socket event already pending, the event could be reported instead of `HttpClientErrc::timeout`. The synchronous I/O driver now rechecks the deadline after polling before accepting readiness. Tests: `aeronet/client/test/http-client-core_test.cpp` (`ReadTimeoutReturnsError`, `Http2ReadTimeoutReturnsError`).
- **HTTP/2 TLS clients now reuse pooled connections that have pending control frames**: the pool previously treated every readable encrypted byte as a stale connection, so a response followed by `WINDOW_UPDATE` traffic caused the next non-empty POST to reconnect and repeat the TLS handshake. Pooled HTTP/2 connections now non-blockingly process complete pending frames before reuse, reject GOAWAY, close, malformed, or partial input before sending the next request, and retain the conservative raw-socket stale check.
- **HTTP/2 request bodies no longer stall when a peer advertises a large initial send window**: new streams previously used the peer's `SETTINGS_INITIAL_WINDOW_SIZE` for both directions. After a peer such as h2load exhausted aeronet's advertised 65,535-byte receive window, the server therefore believed plenty of receive credit remained and sent no `WINDOW_UPDATE`; the peer waited forever for credit before sending `END_STREAM`. Stream send and receive windows now start from the peer and local settings respectively. Tests: `aeronet/http2/test/http2-stream_test.cpp` (`IndependentInitialFlowControlWindows`) and `aeronet/http2/test/http2-connection_test.cpp` (`PeerInitialWindowOnlyControlsNewStreamSendWindow`).
- **Fix race condition in async handle stop**: Fixed the lifecycle shutdown race in `lifecycle.hpp`. `drainDeadlineEnabled` is now atomic, preventing concurrent writes when a worker unwinds from the throwing predicate while AsyncHandle::stop() transitions it to stopping.
- **A throwing `startDetachedAndStopWhen()` / `startDetachedWithStopToken()` predicate no longer crashes on Windows**: a predicate exception is now caught at its call site and treated as a normal stop request (captured for `AsyncHandle::rethrowIfError()`, then reported as `predicate() == true`) instead of unwinding a `std::exception` across the event-loop thread's `runUntilStarted()` RAII guards. This routes every stop through the single, already-covered normal-exit teardown path (closing the listener and active connections on the event-loop thread) instead of a separate, exception-only path. `internal::Lifecycle::state` also now uses acquire/release ordering instead of relaxed, so a controller thread observing `State::Idle` is guaranteed to see the event-loop thread's preceding listener close (a data race confirmed with ThreadSanitizer). Test: `tests/multi-http-server_test.cpp` (`WorkerErrorsAreRetainedAfterStop`).

## Improvements

- **Bounded deferred output and zerocopy retention**: `maxOutboundBufferBytes` now covers HTTP/2 queued frames plus flow-control-deferred in-memory responses instead of only the HTTP/1 write buffer. HTTP/2 pauses input at the wire-output high-water mark, refuses new streams when the connection budget is exhausted, returns 503 for fixed responses that cannot fit, and resets streaming responses that exceed the new per-stream `Http2Config::maxStreamPendingBytes` limit. The new `maxZerocopyPendingBytes` ceiling disables zerocopy on a connection before another retained payload would cross the limit, so delayed error-queue completions plateau while subsequent responses use copied writes. Tests: `http2-connection_test` (`InputPausesAtOutputHighWaterMarkUntilSlowReaderDrains`), `http2-protocol-handler_test` (`*PendingLimit*`, `*ConnectionLimit*`, `SlowReaderOutputPlateausAndLeavesInputUnconsumed`), and `connection-state_test` (`DelayedCompletionsPlateauAndLaterWritesFallBackToCopy`). Normal-path Release A/B runs of `benchmarks/internal/http2-flow-control_bench.cpp` (`BM_ConnectionProcessDataFrames`, `BM_ConnectionBatchSmallResponses`) showed no measurable regression, with stable cases within about 1% of `HEAD`.
- **HTTP/1 CRLF search now delegates to libc `memchr`**: the production scan no longer hard-codes a 128-byte SSE2 prefix whose advantage varied with line length and CPU. It inherits libc's architecture-specific runtime dispatch, while the SSE2 and AVX2 candidates remain in `benchmarks/internal/search-crlf_bench.cpp` for comparison.
- **Response Date headers are cached per reactor**: each `SingleHttpServer` now formats the fixed-width HTTP `Date` header at most once per active wall-clock second and copies the cached 37 bytes into HTTP/1.x, HTTP/2, streaming, redirect, and error responses. The existing event-loop monotonic timestamp drives lazy boundary refreshes, so the request path performs no wall-clock read and idle servers receive no additional timer wakeups. The isolated Release benchmark measures about 0.6 ns for the cached copy versus 35 ns for `now()` plus formatting on the test host.
- **Zero-copy HTTP client response handoff**: completed HTTP/1.1 identity bodies can now transfer the receive allocation into `HttpResponse` while exposing only its body suffix, joining the existing HTTP/1.1 chunked/decompression and HTTP/2 assembly/decompression ownership paths. Suitably-sized scratch rotates into the result with an equal-capacity empty replacement; oversized scratch remains client-owned when the next body is small to avoid duplicate high-water retention. Tests cover empty, identity, chunked, compressed and 1 MiB+ responses.
- **Saturated, repeatable scripted client benchmarks**: the client runner now supports `--repeat N` and reports the complete median-throughput sample. It also separates the client and server onto physical cores and uses several synchronous connections per reserved client CPU. Dedicated prebuilt-response endpoints keep dynamic response generation from saturating the server during client measurements.
- **Batched HTTP/2 receive-window replenishment**: stream and connection credit is restored at a half-window low-water mark instead of emitting two `WINDOW_UPDATE` frames after every DATA frame, and an ended stream receives no useless stream-level update. This substantially reduces frame generation, small TLS writes, and encryption work during bulk transfers.
- **Detailed labeled HTTP/2 telemetry**: metric calls now accept non-owning per-measurement label spans for both OpenTelemetry attributes and DogStatsD tags, without allocating a label container in aeronet. HTTP/2 automatically reports frame counts/payload bytes by direction and type, stream lifecycle/request duration/body-size statistics, and HPACK block sizes/compression ratios. The disabled path uses predictable null checks, existing unlabeled metric overloads retain their prior call shape, `Http2Stream` and per-stream request state do not grow, and `Http2Connection` adds one optional pointer.
- **Header lookup no longer folds case on every comparison**: `HttpMessage` searches normalized names only at header line boundaries using allocation-free `std::string_view::find`. This exploits the rare leading CRLF in header blocks and keeps lookups simpler than a custom Boyer-Moore searcher.
- **Advanced TLS operations and hardening**: servers can passively staple cached DER OCSP responses globally or per SNI certificate, validate inbound client certificates with PEM/DER CRLs or an application revocation hook, and emit NSS-compatible traffic-secret logs in debug builds only. TLS contexts now default to TLS 1.2 or newer, prefer server ciphers, disable renegotiation where supported, enforce a single-label boundary for SNI wildcards, and scrub configuration-owned PEM private keys plus static and runtime session-ticket keys when replaced or destroyed. The MkDocs TLS guide documents response refresh, callback semantics, debugging hazards, and the limits of process-memory zeroization.
- **Adaptive HPACK dynamic-table lookup for enlarged tables**: the encoder now lazily builds a collision-safe name/value hash index at 128 entries, while the default 4 KiB table retains its allocation-free contiguous scan. Comparative 4/16/64 KiB benchmarks keep the cursor-vector over circular and segmented queues on the combined workload; the index avoids its measured 4 KiB maintenance regression while reducing populated-table lookup from 109/395 ns to about 9 ns at 16/64 KiB. Tests: `aeronet/http2/test/hpack_test.cpp` (`LargeDynamicTableIndex*`, `CopiesAndMovesLargeDynamicTableIndex`). Benchmark: `benchmarks/internal/hpack_bench.cpp` (`BM_HpackDynamicTable*`, `BM_HpackFindHeaderByTableSize`).
- **Transport dispatch no longer needs vtables or RTTI probes**: the final `Transport` owner stores its 16-byte `PlainTransport` inline, removing the plain backend allocation and its manual lifetime handling. Plain reads, writes, and capability checks take a direct-call path; TLS and opt-in custom backends remain out of line behind an RAII owner and cached operation table, so OpenSSL remains absent from non-TLS builds.
- **Adaptive HTTP/2 outbound batching**: larger DATA payloads and oversized HPACK blocks retain their backing allocations in a partial-write-safe fragment queue instead of being recopied behind frame headers. Consecutive ordinary HEADERS frames and DATA payloads up to 256 bytes share the connection's existing output buffer, letting TLS encrypt a batch of small responses with one write instead of one `SSL_write` per frame fragment. The shared client/server path still uses `writev` / `WSASend` for cleartext scatter I/O and ordered owned-fragment writes for larger TLS output, including flow-control-deferred streaming responses and uploads. Tests: `aeronet/http2/test/http2-connection_test.cpp` (`SmallResponsesShareOneOutputFragment`, `OwnedDataUsesGatherFragmentsAcrossPartialWrites`, `OversizedHeaderBlockGathersContinuationWithoutRecopy`), `aeronet/sys/test/transport_test.cpp` (`*Gather*`), and `aeronet/client/test/http-client-http2-e2e_test.cpp` (`LargeUploadAndResponseUseH2GatherPath`). Benchmarks: `benchmarks/internal/http2-flow-control_bench.cpp` (`BM_ConnectionBatchSmallResponses`, `BM_ConnectionQueueOwnedDataFragments`).
- **Slightly improved websocket upgrade handler**: minimized allocations and removed a copy.
- **Improved rate limiter performance** by using a sharding on several locks instead of a unique one.
- `Http2Config::maxPriorityTreeDepth` is now enforced - stream priority dependencies exceeding the configured depth (or forming a cycle) are clamped to the root instead of being applied.

## Others

- **Asan is now OFF by default in Debug**. To activate it you will need to explicitly set `AERONET_ENABLE_ASAN=1`
- **Clean-up glaze adapters**: instead of centralizing all glaze adapters in one file far from the objects definitions, move each object glaze adapter code into its own object header file.
- **Fix test expectations in aeronet client when no compression library is available**.

## [1.5.0] - 2026-08-20

### 1.5.0 Breaking changes

- **HTTP CONNECT tunneling is now disabled by default**: an empty `HttpServerConfig::connectAllowlist` now rejects every CONNECT target with `403 Forbidden` instead of allowing every resolved host. This closes an unauthenticated SSRF/open proxy in default configurations across HTTP/1.1 and HTTP/2. Applications that intentionally provide CONNECT tunnels must explicitly configure every trusted target hostname or IP with `withConnectAllowlist()` (or the `connectAllowlist` JSON/YAML setting). Setting the allowlist to `["*"]` deliberately restores the previous unrestricted behavior, allowing every host and port including loopback, private-network, link-local, and cloud metadata targets.
- **HttpRequestView::headerValue, headerValueOrEmpty, trailerValue, trailerValueOrEmpty, hasHeader and hasTrailer** are now all expecting lower ASCII case keys. This is enforced by the new parameter `LowerAsciiKey` that will fail to compile for constant strings (for instance, `headerValue("Host")` does not compile, but `headerValue("host")` does)
- **HttpRequestView::headers() && trailers() now return a case-sensitive map with lower case keys**: header names are normalized to lower-case when parsed, but the returned map have case sensitive look-ups. For instance, the code `req.headers().find("X-Header")` is now wrong (it can never match) and should be replaced with `req.headers().find("x-header")`. For simple lookups, prefer above methods that are safer and simpler.
- **http::Connection, http::ContentType, http::Host, etc. are now `LowerAsciiKey` instead of `std::string_view`** (compile-time validated once at their definition instead of at every call site). Fully backward-compatible for typical usage (implicit conversion back to std::string_view is available) -- only generic/template code asserting the exact type `std::string_view` would need adjustment. Pure formatting helpers (`http::ContentTypeHeaderSep` and similar `*Sep` constants, which are not header-name lookup keys) are unaffected and remain `std::string_view`.
- **File::Identity becomes private and File::identity() has been replaced with File::appendIdentityData()**: `File::Identity` is now a private nested type, and the public `File::identity()` method has been removed. The new `File::appendIdentityData(char* pData)` method writes the file's current descriptor identity and metadata to the provided buffer, returning a pointer past the last written byte. The buffer must be at least `File::kIdentitySize` bytes long.
- **Global header values should be trimmed of OWS**. This is to save some work on the http message finalization path.

### 1.5.0 Bug Fixes

- **Client: heap-buffer-overflow when adding a header to a body-less request built with reserved header capacity**: `HttpClient::makeRequest(additionalCapacity, method, url)` (the overload without a body) was incorrectly computing indexes to its internal buffer.
- **HTTP/1.1 request is now rejected if it does not contain a Host header**: the server now returns `400 Bad Request` for HTTP/1.1 requests that do not include a `Host` header, per RFC 9112 §3.2.
- **Router updates could race server startup**: a route update submitted while the server was preparing to run could mutate the router directly while startup clamped route configuration, causing intermittent literal-route assertions. Startup now publishes a synchronized `Starting` state before launching its thread, so subsequent updates are queued for the event-loop thread.
- **Predicate and stop-token shutdown could leave a listener open without an event loop**: when cancellation arrived between event-loop iterations, `runUntil()` could reset its lifecycle without closing the listener or active connections. New TCP connections then succeeded but were never serviced, most visibly as intermittent 10-second Windows CI stalls. Predicate-driven exits now perform teardown on the event-loop thread, and lifecycle tests use a bounded listener-closure check. Tests: `tests/http-server-lifecycle_test.cpp` (`StartAndStopWhen`, `StartWithStopToken`), `tests/multi-http-server_test.cpp` (`StartDetachedWithStopTokenStopsOnRequest`).
- **Async handlers: heap-use-after-free when a connection was closed during a long `deferWork()`**: a request whose async handler ran slow background work (e.g. a multi-second database query) could have its connection swept by the keep-alive idle timeout while the work was still in flight. When the background thread finished it wrote its result into the coroutine frame / connection memory that had already been freed by the event-loop thread, which AddressSanitizer reported as a `heap-use-after-free`. The fix is threefold: (1) `DeferredWorkAwaitable` now stores the result/exception in a `shared_ptr` state co-owned by the background thread and copies the event-loop post-callback at construction, so the worker never dereferences the `HttpRequestView` or the coroutine frame after completion; (2) connections with an active async handler are excluded from the keep-alive deadline sweep and re-armed once the response is flushed, so an in-flight request is no longer closed as if idle; (3) each connection carries a monotonic `generation` token that is validated before a posted async completion runs its pre-resume work or resumes the coroutine, so a stale completion can never resume a different connection that reused the same fd. Applies to both HTTP/1.x and HTTP/2 async paths. Tests: `aeronet/http/test/http-request-view_test.cpp` (`DeferredWorkCompletionOutlivesAwaitableStorage`), `tests/http-routing_test.cpp` (`DeferredWorkIsNotSweptByKeepAliveTimeout`).
- **Fixed HPACK dynamic table desynchronization**: entries are now always added to the dynamic table on incremental indexing, even when the resulting header is rejected as malformed, keeping compression state in sync with the peer per RFC 9113 §4.3.
- **HTTP/1.1 now rejects field names that aren't valid `token`s (RFC 9110 §5.6.2)**, and field values containing NUL, bare CR, or bare LF (RFC 9112 §2.2), instead of accepting them silently.
- **HTTP/2 now rejects field names containing uppercase ASCII or invalid bytes**, and field values containing NUL/CR/LF, per RFC 9113 §8.2.1, instead of accepting them silently.
- **HTTP/2 now rejects malformed pseudo-header field sections**: duplicate, misordered, undefined, response-only, and missing request pseudo-headers, plus invalid CONNECT pseudo-header shapes, produce a stream-level `RST_STREAM(PROTOCOL_ERROR)` without closing the connection. Invalid HPACK encoding remains a connection-level `COMPRESSION_ERROR`, and malformed field sections are still decoded in full to keep the HPACK dynamic table in sync. Extended CONNECT's defined `:protocol` field is rejected contextually because its setting is not supported. Unsupported extension methods now return `501 Not Implemented` instead of being dispatched as `GET`.
- **HTTP/2 now enforces `SETTINGS_MAX_HEADER_LIST_SIZE` for decoded header fields**: initial and trailing header blocks whose RFC 9113 header-list size exceeds the configured `Http2Config::maxHeaderListSize` are rejected with `RST_STREAM(ENHANCE_YOUR_CALM)` before reaching request handling.
- **A handler slower than `keepAliveTimeout` could get its own response swept away as idle**: the keep-alive deadline is armed when the request is read, so a handler that took longer than `keepAliveTimeout` (a multi-second database query, say) returned with the deadline already expired and the next maintenance sweep closed the connection as if it had been idle - while the response it had just produced was still going out. HTTP/1.1 hands its whole response to the kernel in one go and rarely noticed, but HTTP/2 parks whatever exceeds the peer's flow-control window until a WINDOW_UPDATE arrives: that tail was dropped, and since a downloading HTTP/2 peer keeps sending frames the kernel answered `close()` with RST rather than FIN, so clients got a successful 200 with fewer bytes than `content-length` (`unexpected EOF`) instead of the response the handler returned. `keepAliveTimeout` bounds idleness *between* requests (see `HttpServerConfig::keepAliveTimeout`), so the idle window now restarts when the work completes. Active HTTP/2 streams are excluded from keep-alive reaping until their responses complete, including while waiting for peer flow-control credit; normal idle expiry resumes once no streams remain active. Tests: `tests/http2-core_test.cpp` (`SlowHandlerDoesNotGetItsOwnResponseSweptAsIdle`), `aeronet/client/test/http-client-http2-e2e_test.cpp` (`SlowHandlerLargeResponseSurvivesKeepAliveSweep`).
- Fixed macOS `EventLoop::add()` potentially masking a failed filter registration when adding both `EVFILT_READ` and `EVFILT_WRITE` in one call; each filter's result is now checked individually via `EV_RECEIPT`.

### 1.5.0 Improvements

- **Faster and smaller router dispatch**: literal-route open-addressing slots now store compact side-table indices, and `RoutingResult` references immutable route metadata instead of copying configuration and middleware ranges. The result shrank from 96 to 48 bytes, the hot `Router::match` symbol shrank by 39%, and pinned internal benchmarks show about 45% lower median latency for deterministic literal hits.
- **Faster HTTP/1.1 client response capture**: completed chunked and decompressed bodies now transfer suitably sized scratch allocations into `HttpResponse` instead of making a final body copy, while oversized scratch remains reusable for small bodies. The 256 KiB chunked parser microbenchmark is about **39% faster**.
- **Faster, stricter HTTP/1.1 client response parsing**: chunk-size scanning, hexadecimal decoding, OWS/extension handling, overflow checks, and CRLF consumption now run in one forward pass; status/header/trailer lines use SIMD-accelerated `SearchCRLF`, and chunk-data delimiters use direct validation. The client now rejects bare-LF framing and consistently requires CRLF, matching the server parser. Chunked-response benchmarks are about **25% faster** for 1 KiB chunks and **68% faster** for 16-byte chunks.
- Added a Material for MkDocs documentation site with structured English navigation, local/CI validation, and GitHub Pages deployment alongside the live benchmark dashboards.
- **Replace std::to_chars(int) with faster custom WriteInt**
- **Get rid of `<aeronet/stringconv.hpp>`: removed internal function `StringToTimeISO8601UTC` that now becomes useless.
- **Remove limit of number of settings in SETTINGS frame header in HTTP2**: The HTTP/2 spec does not limit the number of settings in a SETTINGS frame, but aeronet previously limited it to 16. This limit has been removed, and the SETTINGS frame is now parsed according to the spec without any arbitrary limit.
- **Fix MSVC harmless warnings of unreachable code in ndigits.hpp**
- **Fix gcc harmless warnings of sign conversion in ndigits.hpp**
- **Optimized HPACK Huffman decoding** with a smaller lookup table, canonical fallback, and correct padding handling, significantly improving encode/decode performance. Reworked static header lookup using a collision-free perfect hash and optimized dynamic table insertion/eviction to eliminate unnecessary memory moves. Removed redundant Huffman length computation during encoding and reduced memory usage, yielding up to **46% faster decoding**, **30% faster round-trips**, and **63% faster static header lookups**.
- **Benchmark profiling workflow**: `scripts/profile_benchmark.sh` now supports reliable command/PID recording, existing `perf.data` post-processing, SVG flamegraphs, and Hotspot AppImage discovery. Scripted server and client benchmarks can profile each measured workload directly with `--profile`.
- **Simplify and optimize ParseHeaderLine**: use `std::string_view::find` that decays to `std::memchr` for faster colon search, and added force inline to gain an additional 7 % speedup (and no extra code generation). Total expected speedup are roughly **~40%** for typical browser headers, **~30%** for API proxy headers, and **~0%** for short names headers. See `benchmarks/internal/init-try-set-head_bench.cpp` for the new benchmark coverage.
- **Faster HTTP/1 CRLF search**: `SearchCRLF` now scans the first 128 bytes with baseline SSE2 on x86 before falling back to libc `memchr`, improving the realistic request-corpus microbenchmark by about **10%**. Non-SSE2 targets retain the portable `memchr` path. See `benchmarks/internal/search-crlf_bench.cpp` and `aeronet/tech/test/memory-utils_test.cpp`.
- **Faster mime mappings lookup**: `DetermineMIMETypeStr` now uses a binary search on extension codes on 64 bits instead of comparing std::string_views to lowercase. Function gains around 40% efficiency on average.
- **Further request head buffer parsing optimizations**: faster CI hashing for headers, faster request method parsing. Measured gains of ~10% for requests with very few headers, and up to ~23% for requests with many headers. See `benchmarks/internal/init-try-set-head_bench.cpp` for the new benchmark coverage.
- **Improved header & trailer lookups** by normalizing keys to lower case in HttpRequestView.

### 1.5.0 Others

- Bumped `glaze` version to `8.1.0`.

## [1.4.1] - 2026-07-25

### 1.4.1 Bug Fixes

- **Fix compilation with OpenSSL**: cmake configuration did not export publicly the `aeronet_tls` static library.

### 1.4.1 Improvements

- **HTTP client: HTTPS verification failed on minimal Linux images lacking OpenSSL's default CA directory.** When no explicit CA was configured (`tlsCaFile` / `tlsCaPath` empty) and peer verification was on, `HttpClient` relied solely on `SSL_CTX_set_default_verify_paths()`, whose compiled-in directory (often `/usr/lib/ssl`) is frequently absent from minimal container images that ship only a bundle such as `/etc/ssl/certs/ca-certificates.crt`. The trust store was then empty and every handshake failed with `certificate verify failed`. The client now still honours OpenSSL's default paths and the `SSL_CERT_FILE` / `SSL_CERT_DIR` environment variables, but - when neither env var is set - also probes the well-known system CA locations (e.g. `/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt`, `/etc/ssl/certs`) and loads whatever exists, so HTTPS works out of the box. Tests: `aeronet/client/test/http-client-tls-e2e_test.cpp` (`TrustsServerViaSslCertFileEnvWithoutExplicitCa`, `LoadExistingCaBundlesLoadsExistingLocationsAndSkipsMissing`).
- **New `HttpClient::makeRequest` constructors for convenience**: `makeRequest(std::size_t additionalCapacity, std::string_view method, std::string_view url)` useful to add headers before body without reallocations.

## [1.4.0] - 2026-07-23

### 1.4.0 Breaking changes

- **`HttpRequest` has been renamed into `HttpRequestView`** to better reflect its non-owning semantics. `HttpRequest` is now a concrete owning type used in the `HttpClient` API to build a request, mirroring `HttpResponse`.
- **HTTP/2-specific `HttpRequestView` APIs** (`isHttp2()`, `streamId()`, `scheme()`, `authority()`) are now compiled only when `AERONET_ENABLE_HTTP2` is on. Without HTTP/2, use the HTTP/1.x equivalents (e.g. `headerValueOrEmpty("host")` for `authority()`).
- **JSON/YAML body helpers now require `<aeronet/http-json.hpp>`**: `HttpResponse::bodyJson`/`bodyYaml` and `HttpRequestView::bodyAs`/`bodyAsYaml` keep their declarations in the core headers, but their definitions (and the Glaze include) moved to the new opt-in `<aeronet/http-json.hpp>` to keep Glaze's compile cost out of the core. Code using the `<aeronet/aeronet.hpp>` umbrella is unaffected.
- **The `aeronet::log` API now requires `<aeronet/log.hpp>`**: the core headers no longer pull in spdlog transitively, so callers of `aeronet::log::*` must include it directly.

### 1.4.0 New features

- **HTTP client (`HttpClient`, `AERONET_ENABLE_HTTP_CLIENT`)**: a new synchronous HTTP client built on aeronet's own non-blocking transport + event-loop bricks, speaking both **HTTP/1.1 and HTTP/2** natively (HTTP/2 reuses the server's HPACK + frame codecs, adding no dependency; the version is selected per client via `HttpClientConfig::httpVersion` - `Auto` / `Http2` / `Http1_1` - with ALPN negotiation over TLS and prior-knowledge h2c over cleartext). It provides HTTPS through the shared `TlsTransport` (peer/hostname verification, TLS version bounds, custom CA bundle, cipher list, mutual-TLS), per-origin **keep-alive connection pooling**, automatic **redirect following** with RFC 7231 method/body rewriting, a configurable **retry policy** with exponential backoff, transparent **compression / decompression** reusing the server codec bricks, outbound **request trailers** over both HTTP/1.1 (chunked framing) and HTTP/2 (a trailing HEADERS block), **request bodies streamed directly from an open file** (`HttpRequest::file(...)`, with an optional offset/length sub-range) - zero-copy `sendfile(2)` over cleartext HTTP/1.1, read-and-encrypt over TLS, and bounded reads framed into DATA frames over HTTP/2 (tests: `aeronet/client/test/http-client-core_test.cpp` `*FileBody*`, `http-client-http2-e2e_test.cpp` `*FileBody*`, `http-client-tls-e2e_test.cpp` `PostFileBodyOverTls*`, `aeronet/sys/test/transport_test.cpp` `*SendFile*`), cleartext **forward-proxy** support (`withProxy`, opening an HTTP `CONNECT` tunnel for https origins), and an opt-in **time-based response cache** for idempotent requests (`withCache`). Every request returns an `HttpClientResult` (`std::expected<HttpResponse, HttpClientErrc>`), so per-request runtime failures are values rather than exceptions; a single `HttpClient` is not thread-safe (use one per thread). See [HTTP client](https://github.com/sjanel/aeronet#http-client).
- **JWT (JWS profile) module (`aeronet_jwt`, `AERONET_ENABLE_JWT`)**: sign and verify JSON Web Tokens (RFC 7519) across the full JWS algorithm suite (HMAC `HS*`, RSA `RS*`/`PS*`, ECDSA `ES*`, `EdDSA`) over the OpenSSL already linked for TLS. Claim validation (`exp`/`nbf`/`iss`/`aud`/`sub` with configurable leeway + injectable clock), JWK/JWKS parsing with `kid` selection, and a security posture enforced by construction (`alg:none` always rejected, algorithm-family anti-confusion, signature verified before any claim, constant-time HMAC). Exception-free, JWE out of scope, and enabled by default whenever OpenSSL + Glaze are present (a `cmake_dependent_option`, no new dependency). Also ships a url-safe, no-padding `base64url` codec in `aeronet/tech`. See [FEATURES.md](FEATURES.md).
- **Automatic HTTP → HTTPS redirect**: a plaintext listener can answer every request with a 3xx redirect to the equivalent `https://` URL via `HttpServerConfig::withHttpsRedirect(targetHttpsPort = 443, statusCode = 301)`. The redirect host derives from the request `Host` header; the path and query are re-encoded so `Location` is always a valid, injection-safe URL; a request with no `Host` gets `400`. Cannot be combined with `tls.enabled` on the same listener.
- **HTTP request rate-limiting middleware**: `RateLimitRequestMiddlewareBuilder` with a configurable client-key strategy (peer address, `X-Forwarded-For`, custom header, or custom extractor). The default in-memory token-bucket backend returns `429 Too Many Requests` with `Retry-After`, and installs globally (`Router::addRequestMiddleware`) or per-route / per-`RouteGroup`. Ships an optional Redis sliding-window contract (`RedisSlidingWindowRateLimitStore`, exposing the Lua script payload and a deterministic key schema) for distributed multi-instance synchronization.
- **Dedicated probe listener for `MultiHttpServer`**: `BuiltinProbesConfig::withDedicatedPort(port)` serves `/livez` / `/readyz` / `/startupz` from a separate single-threaded event loop, isolating probe availability from worker load. Liveness is heartbeat-based - each worker publishes a heartbeat at the top of every loop iteration, and the pod is reported unhealthy only when **every** worker's heartbeat has been stale beyond `livenessStaleThreshold` (default `10s`), so a busy-but-progressing worker stays live while a full deadlock trips it. Opt-in (`dedicatedPort == 0`, the default, keeps probes inline). See the [Kubernetes deployment guide](kubernetes-examples.md#isolating-probes-on-a-dedicated-port).
- **Client address on the request**: `HttpRequestView::clientAddress()`, populated from the peer socket address for both HTTP/1.1 and HTTP/2.
- **HTTP/2 request trailers**: the server now accepts a trailing `HEADERS` block on a request stream (RFC 9113 §8.1) and surfaces its fields through `HttpRequestView::trailers()` / `trailerValueOrEmpty()`, exactly like HTTP/1.1 chunked trailers. Pseudo-header fields in trailers and a trailing block that does not end the stream are rejected with `RST_STREAM(PROTOCOL_ERROR)`; trailer bytes count toward the request header-size budget. Tests: `aeronet/http2/test/http2-protocol-handler_test.cpp` (`RequestTrailers*`), `aeronet/client/test/http-client-http2-e2e_test.cpp` (`*RequestTrailer*`, `LargeBodyThenTrailers`, `ReusableAfterTraileredRequest`).

### 1.4.0 Bug fixes

- **Request trailers could leak across keep-alive requests**: after a chunked request carrying trailers, the per-connection trailer length was not reset for a subsequent fixed-length request on the same connection, so the server spuriously re-parsed the previous request's trailer bytes into `HttpRequestView::trailers()`. The trailer length is now cleared for non-chunked bodies.
- **HTTP/2 responses larger than the peer's flow-control window could never be delivered** - a buffered response body exceeding the stream/connection send window (e.g. > ~64 KiB against the RFC 9113 default window) failed with `FLOW_CONTROL_ERROR` and hung; the window-exhausted remainder is now deferred and flushed as `WINDOW_UPDATE`s arrive, trailers included.
- **HTTP/2 server leaked active-stream accounting** for responses completed outside frame processing (flow-control-deferred bodies, file payloads flushed from `onOutputWritten`, async completions), so a long-lived connection eventually died with `Max concurrent streams exceeded`.
- **HTTP/2 responses could carry field values with trailing whitespace**, which strict peers (nghttp2, hence curl) reject per RFC 9113 §8.2.1 - so every compressed response over HTTP/2 failed for such clients. Header values are now OWS-trimmed before HPACK encoding.
- **Large TLS responses could be truncated by ~one 16 KiB record under load** - a fatal error left on the shared per-thread OpenSSL error queue by `read` / handshake was misclassified against the next connection's benign would-block write, dropping a still-pending TLS record. Both paths now drain the queue on every fatal return.
- **mTLS: `requireClientCert` could be silently ignored** when `requestClientCert=false` (reachable via config), so certificate-less clients were accepted; the invariant `requireClientCert ⇒ requestClientCert` is now enforced on every configuration path.
- **Empty-body HTTP/1.1 responses omitted `Content-Length`**, defeating keep-alive reuse; `Content-Length: 0` is now synthesized (excluding statuses that must not carry it, HEAD, and file / streaming / direct-compression responses).
- **Response status-line omitted the mandatory SP before an empty reason-phrase** (`HTTP/1.1 200\r\n`), which strict RFC 9112 parsers may reject; aeronet now always emits the separator (`HTTP/1.1 200 \r\n`).
- **glaze JSON/YAML reads could over-read a non-null-terminated body buffer** - `HttpRequestView::bodyAs<T>()` / `bodyAsYaml<T>()` now parse with `null_terminated = false`.
- **Out-of-bounds read on a stale poll event (rare macOS-only crash)** - `ConnectionStorage::iterator(fd)` now maps any out-of-range fd to `end()` instead of dereferencing past the (shrunk) connection vector.
- Fixed a Debug-only compilation error in `aeronet::fullVersionStringView()` (`<aeronet/version.hpp>`).
- Added `Content-type` header for built-in server error messages with a body.

### 1.4.0 Improvements

- **Raised status line max size from 64KiB to 16MiB** - this is not so useful for `HttpResponse` because reason should not be used, but may be interesting for very long URLs in `HttpRequest`.
- **Lower compile-time footprint of the core HTTP headers**: the two compile-time heaviest dependencies - Glaze and spdlog - no longer leak into the widely-included headers, so most translation units stop paying for them even when they never serialize JSON or log (see the header breaking changes above).
- **Pre-computed static-file response headers (`StaticFileHandler`)**: the per-file `ETag` / `Last-Modified` / `Content-Type` fragments are formatted once and cached, validated on every request against file size + mtime (both read from the `fstat()` already performed on open, which also removes a redundant stat per request). Bounded LRU cache via `StaticFileConfig::headerCacheCapacity` (default `1024`, `0` disables).
- **Faster `CaseInsensitiveEqual`** - header-name matching, one of the hottest server operations, now compares 16 bytes at a time (SSE2 on x86-64, NEON on ARM): ~35–57% faster than the previous 8-byte SWAR on strings ≥ 16 bytes.
- **Faster small `Copy` / `Append`** (`memory-utils.hpp`): runtime-sized copies of ≤ 32 bytes use a couple of inline overlapping stores instead of `std::memcpy` (~1.9x on isolated small copies, ~1.5x on a realistic HTTP fragment mix).
- **`HttpServerConfig::nbThreads` narrowed to `std::uint16_t`** (was `std::uint32_t`): one thread maps to one event loop, so the 16-bit range documents the valid bound. JSON/YAML values above 65535 are now rejected; source-compatible for normal use.
- Increased the maximum length of **reason** in `HttpResponse` from 1024 to 65522.
- **Split the library into two CMake targets**, `aeronet_server` and `aeronet_client`, plus a new `aeronet` umbrella interface library that pulls in both (existing `<aeronet/aeronet.hpp>` code is unaffected).
- Added scripted **HTTP/2 client benchmarks** to CI (aeronet `HttpClient` vs libcurl over `h2c` and `h2`-TLS), published per protocol to GitHub Pages with matching badges.
- Better memory reservation for HPACK headers in HTTP/2 instead of a fixed 512-byte reserve.
- Improved `StaticFileHandler` header metadata cache by changing the O(n) search for cache eviction into a O(1) LRU linked-list with a hash map for fast lookup.

## [1.3.0] - 2026-06-01

### 1.3.0 New features

- **Route parameter constraints**: Router path params now support inline constraints with `{name:pattern}` syntax (for example `/users/{id:[0-9]+}`). Constraint patterns are compiled at registration and validated during matching. The engine uses a fast custom matcher for common character-class patterns and falls back to `std::regex` for complex expressions.
- **Network fault injection test infrastructure** (`aeronet/test_support/basic/`): `FaultPolicy`, `TestPipe`, `TestTransport`, and `FaultInjectingTransport` provide deterministic transport-level fault simulation for both unit and integration testing. A global transport decorator hook (`g_transportDecorator` in `transport-test-hook.hpp`) lets integration tests wrap real transports with fault injection at accept time. The hook is compile-time gated (`AERONET_ENABLE_TEST_HOOKS`, set automatically when `AERONET_BUILD_TESTS=ON`) - zero overhead in production builds. Tests: 26 unit tests + 11 integration tests covering partial delivery, EAGAIN, resets, combined faults, and pipelining.
- Configuration of `HttpServer` by yaml or json config files with `AERONET_ENABLE_GLAZE` (see `SingleHttpServer::SingleHttpServer(const std::filesystem::path&)` and `MultiHttpServer::MultiHttpServer(const std::filesystem::path&)` constructors).
- **Structured access logging**: Added minimally configurable access logs via `HttpServerConfig::accessLog` with formats `clf` and `json`, sinks `none`, `stdout`, and `file`, and optional `X-Forwarded-For` client IP extraction. Implementation is independent from optional spdlog and uses an aeronet-native buffered writer.
- **Adaptive event-loop poll timeout**: `HttpServerConfig::pollIntervalMinFactor` / `pollIntervalMaxFactor` (default `1.0F` / `1.0F`) scale `pollInterval` dynamically. Saturated polls drop to the min factor; consecutive idle polls back off exponentially up to the max factor. Builder: `withPollIntervalFactors(minFactor, maxFactor)`. The default `{1, 1}` keeps the previous fixed behavior.
- **Keep-alive deadline queue**: idle keep-alive reaping now uses an intrusive min-heap keyed by expiry deadline instead of scanning every active connection on each maintenance tick. The common idle HTTP/1.1 path checks only expired deadlines, with full sweeps reserved for active timeout/backpressure/drain maintenance. Added `aeronet-bench-internal-keep-alive-deadline-queue` for 10 K idle-connection measurements.
- **Configurable accept batch size**: New `HttpServerConfig::maxAcceptBatchSize` (default: 64) controls how many new connections are accepted per event-loop iteration. Prevents connection-burst starvation of existing connections under high concurrency.
- **Per-route configuration**: `PathEntryConfig` now supports `requestTimeout`, `maxBodyBytes`, and `maxHeaderBytes` overrides. Chainable setters on `PathHandlerEntry` (`.timeout()`, `.maxBodyBytes()`, `.maxHeaderBytes()`) allow per-route configuration inline with handler registration.
- **Per-route body limit enforcement**: Per-route `maxBodyBytes` is enforced server-side across HTTP/1.1 (sync and async paths) and HTTP/2, returning `413 Content Too Large` when the decoded body exceeds the route-specific limit.
- **Per-route header limit enforcement**: Per-route `maxHeaderBytes` is enforced after routing in both HTTP/1.1 and HTTP/2, returning `431 Request Header Fields Too Large` when headers exceed the route-specific limit (tighter than the global limit already enforced during parsing).
- **Per-route request timeout**: Per-route `requestTimeout` sets a handler deadline enforced during periodic sweeps. For HTTP/1.1, the deadline is checked connection-wide; for HTTP/2, per-stream deadlines are checked independently via `sweepStreams()`. Expired requests receive `408 Request Timeout`.
- **Route groups** (`RouteGroup`): Lightweight non-owning prefix proxy created via `Router::group(prefix)`. Supports shared configuration (timeout, body/header limits, HTTP/2 enable), CORS policy, and request/response middleware applied to all routes in the group. Groups can be nested with config inheritance, and per-route overrides take precedence over group defaults.

### 1.3.0 Bug fixes

- `flat_hash_map`: fix `erase(iterator)` return semantics when erasing a colliding entry relocates another chained element into the erased slot. The returned iterator now points to the relocated element instead of advancing past it, which fixes erase-while-iterating loops that could skip matching entries under collisions.
- **HTTP/1.1 parser: fix header boundary off-by-one** - `initTrySetHead` could misidentify a complete request when the input buffer ended exactly at a header CRLF boundary (e.g., `"Host: test\r\n"` as the last byte). The header-parsing loop exited via its for-condition (`first >= last`) without finding the empty line terminator, then fell through to compute `headSpanSize = bufferSize + 2`. The subsequent `erase_front(headSpanSize)` would assert or corrupt state. Fixed by tracking whether the empty-line terminator was actually found and returning `kStatusNeedMoreData` when it was not.
- WebSocket: fix `processInput()` consumed-byte accounting when carry-over input from a previous incomplete frame exists. `bytesConsumed` now reports only bytes consumed from the current caller buffer (excluding already-buffered carry-over), preventing over-erasing the connection input buffer and avoiding `RawBytesBase::erase_front` assertion failures under high-load.
- Fix connection stall when `maxPerEventReadBytes` fairness cap interrupts a read with data still in the TCP buffer. Edge-triggered polling (EPOLLET on Linux, EV_CLEAR on macOS) does not fire a new read event on a non-empty -> non-empty buffer transition, causing the connection to hang until keepAlive timeout. The fix defers partially-read fds and re-processes them at the start of the next event-loop iteration.
- Fix tunnel connection hang when `epoll_ctl(EPOLL_CTL_MOD)` fails (e.g. `EACCES` on ARM/Linux CI runners): `forwardTunnelData` now propagates the `enableWritableInterest` failure so the tunnel is closed immediately instead of looping indefinitely between data arrival and deferred drain. Also clear buffered write state on `EventLoop::mod()` failure so `canCloseConnectionForDrain()` can proceed.
- Honor `HttpServerConfig::minReadChunkBytes` for actual transport reads: `ConnectionState::transportRead()` now uses `chunkSize` as the syscall read length instead of reading the whole currently available buffer capacity. This makes chunk sizing deterministic on long-lived connections and keeps `maxPerEventReadBytes` fairness behavior predictable.
- HTTP/2: fix stream admission check for peer-initiated HEADERS by enforcing local `SETTINGS_MAX_CONCURRENT_STREAMS` (instead of peer settings), preventing incorrect acceptance/rejection of new incoming streams.
- HTTP/2: do not send an empty HEADERS frame for deferred file sends without trailers

### 1.3.0 Breaking changes

- `HttpServerConfig::maxHeaderBytes` changed from `std::size_t` to `std::uint32_t`. No HTTP implementation should need >4 GiB of headers; this change eliminates struct padding and aligns with `PathEntryConfig::maxHeaderBytes`. Callers passing `std::size_t` values may need a narrowing cast.
- `HttpServerConfig::withMaxHeaderBytes()` parameter changed from `std::size_t` to `std::uint32_t`.
- `HttpServerConfig::maxPerEventReadBytes` no longer treats `0` as unlimited. It must now be `> 0`; use `std::numeric_limits<uint32_t>::max()` to approximate unlimited behavior.
- Default `HttpServerConfig::maxPerEventReadBytes` changed from `0` (previous unlimited mode) to `128 KiB` to enforce fairness by default.

### 1.3.0 Improvements

- Optimized WebSocket frame building by removing some copies and allocations.
- Slightly improved Router test coverage
- Decrease looping allocations in `HttpResponseWriter` by reusing a single buffer for compression
- HTTP/2 HPACK static table: hash-based encoding lookup (expected CPU gain from internal benches: ~33.5% average on `BM_HpackFindHeader` across 0/10/50/100 dynamic entries)
- HTTP/2 HPACK dynamic table: reuse evicted-entry buffers in `add()` (expected CPU gain from internal benches: ~46.9% average on `BM_HpackEncode*`, ~19.9% average on `BM_HpackRoundTrip*`; decode overall roughly flat on average)
- HTTP/2 stream cleanup: consolidate per-stream maps for better cache locality
- Decrease memory usage in connections by offloading async handler states to a separate object that is not allocated for connections that do not use async handlers.
- Backpressure queueing now rejects additional response chunks immediately once `maxOutboundBufferBytes` marks a connection for drain, avoiding repeated buffer growth/work when streaming handlers ignore a failed `writeBody()` result.
- **WebSocket output zero-copy: write directly into caller-provided buffer** - Instead of accumulating frames in `_outputBuffer` and then draining it, let the protocol handler accept a caller-provided `HttpMessageData&` and write frames directly into it, bypassing `_outputBuffer` entirely. Eliminates both the intermediate allocation and the drain copy for single-frame responses. Requires a larger interface change (pass the destination buffer into `processInput` or a separate `buildOutput` step). Build on the above move-ownership idea first.

### 1.3.0 Other

- Added **WebSocket** benchmarks to CI pipeline with k6 scenarios (echo, mix, ping-pong, churn, compression) comparing aeronet, uWebSockets, and Drogon.
- Fix test `http2-protocol-handler_test` when `AERONET_ENABLE_ZLIB` is off and `AERONET_ENABLE_HTTP2` is on.

## [1.2.0] - 2026-03-23

`aeronet` is now available for:

- **macOS** with a kqueue-based event loop backend.
- **Windows** with an WSAPoll-based event loop backend.

### 1.2.0 Breaking changes

- `HttpServerConfig::tcpNoDelay` becomes an enum with `Auto` mode added instead of a simple boolean. The default value is `Auto` to activate `TCP_NODELAY` for HTTP/2 with TLS connections that benefit the most from it, while keeping it disabled for other connections. `HttpServerConfig::withTcpNoDelay()` stays unchanged.

### 1.2.0 Bug fixes

- `HTTP/2`: Fix `:path` header parsing to correctly extract query parameters mirroring HTTP/1.1 parsing behavior and perform in-place percent-decoding of both path and queries.
- `Router`: Fix an assertion failure crash when a requested path shares a prefix with a registered path but diverges at a segment that does not match any registered child or wildcard.
- Malformed CRLF at the end of a chunked body of an HTTP request now correctly returns HTTP error 400 instead of waiting for more data.
- Prevent OpenSSL per-thread error-queue leakage: call `ERR_clear_error()` after `SSL_shutdown()` in `TlsTransport::shutdown()` to avoid stale errors from a closed connection being misclassified as fatal on subsequent `SSL_read_ex`/`SSL_write_ex` calls (fixes connection recycle/reuse issue).
- Strictly respect `CompressionConfig.maxCompressRatio` in automatic compression even when HttpResponse allocated buffer has more room than the maximum compressed body size.
- Do not consider `ENOBUFS` (*No buffer space available*) as a fatal error for **Zerocopy** responses, and fallback to non-zerocopy path instead (e.g. for small files or when the kernel runs out of resources).
- **Fix data corruption with Zerocopy mode under sustained load** (Kubernetes / virtual network devices). Drain the kernel error queue (completion notifications) before each `MSG_ZEROCOPY` send to prevent resource exhaustion and pinned page accumulation that caused intermittent data corruption with large payloads. Also fix `ENOBUFS` handling for TLS+kTLS zerocopy path.
- Fix race condition in multi threaded HttpServer when calling `stop()`.
- Do not sweep tunnel connections

### 1.2.0 New features

- **HTTP/2 CONNECT tunneling** (RFC 7540 §8.3): Full per-stream tunnel support with bidirectional DATA frame forwarding, upstream TCP connections managed by the event loop, connect allowlist enforcement, and graceful cleanup on stream reset / connection close.
- **HTTP/2 truly asynchronous handlers**: `AsyncRequestHandler` coroutines that use `co_await req.deferWork(...)` now suspend and resume truly asynchronously on HTTP/2, just like HTTP/1.1. Previously, HTTP/2 async handlers were drained synchronously in a tight loop, blocking all other streams on the connection while a `deferWork` operation was in progress. Each HTTP/2 stream now owns its async task independently; when a coroutine suspends, other streams on the same connection continue to be processed.
- **Multipart / multiple-range responses** (`multipart/byteranges`) support (RFC 7233 multi-range): Full `206 Partial Content` with `multipart/byteranges` MIME body. Ranges are sorted, coalesced, and safety-limited (`maxMultipartRanges`, `maxMultipartBodySize`). See `StaticFileHandler` and `StaticFileConfig`.
- `StreamingHandler` now fully supports **HTTP/2**

### 1.2.0 Improvements

- Replaced connections map with a simple vector, where the index of a connection object is simply indexed by its fd value.
- Removed memmove overhead in **HTTP/2** body handling for non-prepared `HttpResponse`. (a prepared `HttpResponse` is when constructed with `HttpRequestView::makeResponse()`).
- Improved `StaticFileHandler` performance
  - **Small file optimization**: files smaller than a configurable threshold (default 128 KiB) are now read into memory and served as inline bodies instead of using the zero-copy transport path (e.g. `sendfile` on Linux). This can significantly reduce latency for small files by avoiding the overhead of setting up zero-copy transfers, while still benefiting from zero-copy for larger files.
  - Other optimizations in directory listing, file metadata retrieval, `sendfile` chunk size optimization.
- Optimized automatic compression for large bodies by starting a streaming compression with a small initial chunk size and exponentially increasing it, which allows to start sending compressed data to the client sooner and reduce latency, while still being efficient for large payloads.
- Add `HttpServerConfig::zerocopyMinBytes` to configure the minimum HTTP response size for which to use zerocopy transport, default is 128 KiB.
- Added some telemetry counters for automatic `HttpResponse` compression (`aeronet.http_responses.compression.*`).
- Improve Brotli one-shot compression performance by reusing encoder state across calls.
- Do not over allocate memory in `HttpResponseWriter` automatic compression by chunks for `brotli` and `zstd` encoders.
- Reuse `zstd` contexts for one-shot decompressions for better performance.
- Added `WebSocketDeflate` configuration validation to fail early if the provided parameters are invalid (e.g. `windowBits` out of range) instead of silently ignoring them and causing unexpected behavior.
- Less calls to `std::chrono::steady_clock::now()` in the main event loop for better performance
- Security hardening for HTTP/2.
- Optimized char buffer search for '\r\n' by using `std::memchr` instead of `std::search` in `SearchCRLF` utility function, which is a hot path in HTTP parsing.
- Optimized hpack HTTP/2 static header name lookup by using binary search instead of linear search.
- Update metrics in HTTP/2

### 1.2.0 Other

- Migrated from classic **zlib** to **zlib-ng** (native mode) for gzip/deflate compression and decompression. zlib-ng provides SIMD-optimized deflate/inflate with identical wire-format compatibility (RFC 1950/1951/1952). The migration covers the core encoder/decoder, **WebSocket** permessage-deflate, and scripted benchmark servers (C++ helpers and Rust `flate2` backend). The compile flag `AERONET_ENABLE_ZLIBNG` can be set to `OFF` to stay with classic zlib if needed, but zlib-ng is now the default and recommended option for better performance.
- Bumped `zlib` dependency to version **1.3.2**.
- Added new function `fullVersionWithRuntime()` that returns a string with the full version of the library including runtime information (with brotli version).
- Bumped `clang-format` version to **21**.
- Fix: Resolve race in TLS handshake handling that caused a flaky test.
- Addition of **HTTP/2** scripted benchmarks in the CI pipeline, with `h2load`.
- Fixed compilation of `aeronet` with `AERONET_ENABLE_ZLIB` off but `AERONET_ENABLE_WEBSOCKET` on.

## [1.1.0] - 2026-02-16

### 1.1.0 Bug fixes

- Correctly update the `Content-Length` header when using `bodyAppend()` on `HttpResponse` from captured body.
- Correctly format the `HttpResponse` when using **HEAD** method with **trailers** (previously erroneously kept the full payload).
- **HEAD** responses with trailers now correctly omit the body as per RFC 7230 Section 4.3.2, and do not switch to chunked encoding.
- `Accept-Encoding` header parsing is now closer to RFC 7231 Section 5.3.4 when duplicate encodings with different `q` values are present and picks the highest `q` value.
- `h2c` (HTTP/2 cleartext) connections now properly handle the HTTP/1.1 Upgrade mechanism and switch to HTTP/2 after the initial request.

### 1.1.0 Breaking Changes

- Minor validation enforcement: `HttpServerConfig::globalHeaders` now MUST be key value separated by `http::HeaderSep`.
- Removed `telemetryContext()` methods from `HttpServer` and `SingleHttpServer`. You can construct a custom `TelemetryContext` instead if needed.
- Telemetry metric methods (including `DogStatsD` ones) are no more `const` qualified (see why in [improvements](#110-improvements) section).
- Check at runtime if header name and value about to be inserted in a response are valid, otherwise throws `std::invalid_argument`
- HttpResponse constructor with concatenated headers throws `std::invalid_argument` if expected format is not respected.
- `HttpRequestView` query parameter API changed: `queryParams()` no longer returns the non-alloc iterable range - it now exposes a map-like view over parsed query parameters where duplicate keys are collapsed (last-value wins). The previous iteration semantics (preserve duplicate order) are available via the new `queryParamsRange()` method. If you used `queryParams()` with **structured bindings** and that there were no **duplicate** keys in your URLs, **no code change is needed**.
- Previously indicated as **undefined behavior**, setting `Content-Type` and `Content-Length` is now prohibited using the `header` and `headerAddLine` methods.
  You should use the dedicated (already existing) `contentType()` and `contentLength()` methods instead for streaming handlers, and set `content-type` along with the body for normal handlers, otherwise `std::invalid_argument` is thrown.

### 1.1.0 New Features

- **Direct compression**: Inline response bodies created via `HttpRequestView::makeResponse()` can now be compressed at `body()` / `bodyAppend()` call time, before finalization. This is controlled by `DirectCompressionMode` (`Auto`, `Off`, `On`) and configured via `CompressionConfig::defaultDirectCompressionMode`. See [Direct Compression](FEATURES.md#direct-compression-inline-body-streaming-compression) for details.
- `HttpRequestView::makeResponse()` factory methods for simplified response creation with body and content-type.
- `HttpRequestView::deferWork()` method to let the main thread come back to the event loop and launch an asynchronous task (in a dedicated thread) to process the request.
- `size` / `length` method helpers in `HttpResponse`, with `reserve` and capacity getters.
- Option `HttpServerConfig::addTrailerHeader` to automatically emit `trailer` header when trailers are added to responses in `HTTP/1.1` only.
- `HttpRequestView` now exposes a new map-like for query parameters: `queryParams()` which collapses duplicate keys (last-value wins)
- `HttpRequestView` gains the following methods: `hasHeader(key)`, `hasTrailer(key)`, `hasPathParam(key)`, `hasQueryParam(key)`, `pathParamValue(key)`, `pathParamValueOrEmpty(key)`, `queryParamValue(key)`, `queryParamValueOrEmpty(key)`, `queryParamInt(key)`, `headerRemoveLine(key)`, `headerRemoveValue(key, value)`.
- Router paths now accepts asterisk as non terminal segments which are matched as a literal (e.g. `/files/*/metadata`).

### 1.1.0 Improvements

- All Header values stored in `HttpResponse` and `HttpResponseWriter` are now **trimmed** of leading/trailing whitespace on set.
- `DogStatsD` is now able to **reconnect automatically** if the UDS socket becomes unavailable. The client is also more efficient.
- `WebSocketConfig.maxMessageSize` is now **strictly respected** when decompressing a `WebSocket` message
- Optimized *prepared* (built from `makeResponse()`) `HttpResponse` to avoid allocating body and trailers memory for **HEAD** requests.
- **Faster case insensitive hash using FNV-1a algorithm for header name lookups**, and optimized version of `tolower` - Use [City hash](https://github.com/google/cityhash/tree/master) elsewhere (for standard strings)
- `HttpRequestView::queryParamsRange()` satisfies the **C++20 range** concept.
- Reuse encoders contexts instead of recreating them on each request for better performance.
- Faster `HttpResponse::file()` by optimizing body headers update.
- Smaller memory reallocations when using captured body in `HttpResponse`.
- **`MSG_ZEROCOPY` support** for plain text and kTLS transport TCP connections on Linux (with fallback path). Configurable via `HttpServerConfig::withZerocopyMode()` with modes: `Disabled`, `Opportunistic` (default), `Enabled`.
- **`Router` now uses a more efficient path matching algorithm with a radix tree structure**, **it gains around - 40%** in pattern based routes matching speed. Handlers also consume less memory.
- **Reuse** codec contexts in automatic compression / decompression for better performance.
- `HttpResponse::bodyAppend` now reserves memory exponentially to reduce the number of reallocations when appending large bodies in multiple calls.
- **`HttpResponse::body()` capture overloads now require rvalue references**: `body(std::string&&, ...)` and `body(std::vector<std::byte>&&, ...)`. Passing an lvalue `std::string` (e.g. `resp.body(myString)`) now selects the inline `std::string_view` overload. This should not break existing code and would avoid silent copies when the caller passed an lvalue string / vector to the body capture overloads.
- Automatic compression process has been optimized, especially for captured payloads which are now compressed in-place without memory moves.
- Invalid chunked transfer encoding length of queries now return `400 Bad Request` instead of `413 Payload Too Large`
- Decrease memory usage in automatic compression / decompression by using only one shared buffer by server instead of by connection for non-async handlers.

### 1.1.0 Other

- Support of ARM64 (aarch64) architecture in CI builds and tests.
- Added [Crow](https://github.com/CrowCpp/Crow) to benchmark comparisons in CI.
- Added `body-codec` scenario to scripted servers benchmarks in CI.
- New compile time option `AERONET_ENABLE_ASYNC_HANDLERS` to enable asynchronous request handlers (enabled by default)
- Fix test compilation when `AERONET_ENABLE_WEBSOCKET` is on but `AERONET_ENABLE_ZLIB` is off.
- In scripted servers benchmarks, `all-except-python` (default) server selection now excludes `python` server by default to avoid skewing results (`all` and `python` are still available).
- Experimental support for **C++20 modules** by creating a `aeronet` module interface file.
- Refactored some system calls in `aeronet/sys` directory to prepare for future multi-platform support (currently Linux only).
- Fixed benchmarks to take timeouts into account
- Increased default `CompressionConfig::maxCompressRatio` from `0.5` to `0.6` to be more permissive with compression ratio (while still protecting against compression bombs). You can of course still configure it to a lower value if you want to be more strict.
- Decreased benchmarks default number of connections to **50** per thread (instead of **100**) to reduce timeouts and get more accurate latency measurements in high concurrency scenarios.
- Split scripted servers benchmarks into 2 configurations - one with a high number of connections, one with a low number of connections - to get more accurate measurements in both low and high concurrency scenarios.

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
- [x] Unified handler API (same HttpRequestView type for both protocols)
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
