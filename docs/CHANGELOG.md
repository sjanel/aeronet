# CHANGELOG

All notable changes to aeronet are documented in this file.

## Unreleased

### 1.8.0 Highlights

The most important changes. Upgrade especially if one of these applies to you:

- **New client modules**: `WebSocketClient` (`ws://`, `wss://`), cookies in `HttpClient`, HTML parsing with CSS selectors (`AERONET_ENABLE_HTML`), JavaScript pages (`AERONET_ENABLE_JS`) and Protocol Buffers bodies (`AERONET_ENABLE_PROTOBUF`).
- **Security**: an open redirect through paths starting with `//`, a `Host` header copied into the HTTPS redirect, request smuggling through HTTP/2 connection-specific fields, credentialed CORS granted to the `null` origin, compression bombs inflated before `maxExpansionRatio` applied, WebSocket compression bombs left allocated per connection, a crafted HPACK string length crashing the process, an out-of-bounds read on `Sec-WebSocket-Key`, and the HTTP/2 Rapid Reset, MadeYouReset and empty frames floods.
- **Server crashes and freezes**: a route timeout during deferred work corrupted memory, a route timeout on a stalled HTTP/2 connection or an invalid `Origin` stopped the server, and a file shrinking while sent, a failing `epoll_wait()` or a large chunked upload kept the event loop busy.
- **Corrupted response framing**: a streamed body not matching its `contentLength()`, a compressed streamed response announcing the uncompressed length, bodies sent with `204`, `304` or a `HEAD` error, and large incompressible bodies truncated by br and zstd.
- **HTTP/2**: `settingsTimeout`, `pingInterval`, `pingTimeout`, `maxStreamsPerConnection` and `enablePriority` were ignored, drains and graceful `GOAWAY` dropped streams, and many frame sequences desynchronized the HPACK state, failing the whole connection.
- **Performance and memory**: HTTP/1.1 over TLS no longer waits 40 ms for delayed acknowledgments, one system call less per TLS request, idle TLS connections take a third of the memory, HTTP/2 requests take 8 to 23% fewer cycles, async handlers allocate less.
- **Packaging**: installed aeronet packages (CMake install, vcpkg, Conan) now work for their consumers.

### 1.8.0 Breaking changes

#### Behavior

- **kTLS is off by default**: `TLSConfig::ktlsMode` defaults to `Disabled` (was `Opportunistic`). On three GitHub runner CPUs, kTLS send served in-memory bodies of 64 KiB to 1 MiB 18 to 54% slower than OpenSSL in user space; only files sent with `sendfile()` gained. A server sending mostly large files can enable it with `withTlsKtlsMode(TLSConfig::KtlsMode::Opportunistic)`. See [Kernel TLS](protocols/tls.md#kernel-tls-ktls).
- **Stricter request validation (`400`)**: an encoded slash (`%2F`) in the path (it was routed as a separator), a repeated singleton field (`Authorization`, `Proxy-Authorization`, `Content-Type`, `Range`, `If-Range`, `If-Modified-Since`, `If-Unmodified-Since`, `Referer`, `From`, `Max-Forwards`, over HTTP/2 a `PROTOCOL_ERROR` reset), a fragment in the request target, a `Host` (or `:authority`, or authority of an absolute-form target) that is not `uri-host [":" port]` (the HTTPS redirect copied it into its `Location`), a `CONNECT` host that is not a reg-name or a bracketed IPv6 address, an HTTP version other than `HTTP/` digit `.` digit, a `TRACE` request with content, and more fields in request trailers (`proxy-authorization`, `proxy-authenticate`, `www-authenticate`, `max-forwards`, `if-range`, `connection`, `upgrade`). See [Requests](guides/requests.md#query-parameters), [Header names](guides/requests.md#header-names) and [Host header field](guides/requests.md#host-header-field).
- **New HTTP/2 limits, on by default**: `pingInterval` defaults to 30 s (was off), the new `streamIdleTimeout` (60 s) and `maxResetStreams` (1000) apply, and so do `settingsTimeout` and `maxStreamsPerConnection`, which were ignored (see the HTTP/2 fixes). See [Connection lifetime](protocols/http2.md#connection-lifetime).
- **At most 5 stacked content codings are decoded** (`415` beyond), for requests and for `HttpClient` responses, as curl does since CVE-2023-23916. See [Request body decompression](guides/compression.md#request-body-decompression).
- **`StaticFileConfig::maxMultipartBodySize` defaults to 1 MiB** (was 32 MiB; beyond, the full file is sent with `200`), and `validate()` throws when it or `inlineFileThresholdBytes` exceeds `StaticFileConfig::kMaxInMemoryBodyBytes` (2 MiB). See [Ranges](guides/static-files.md#ranges).
- **A directory with an index file is redirected to its URL with a slash** (`/docs` to `/docs/`, `301`), against which its relative links resolve. See [Mapping paths to files](guides/static-files.md#mapping-paths-to-files).
- **HTTP/1.0 requests are answered with an `HTTP/1.1` status line** (RFC 9110 §2.5) and an explicit `Connection: close` or `Connection: keep-alive`, the HTTP/1.0 semantics being kept. See [Connections](reference/http-conformance.md#connections).
- **Compressed responses carry a weak `ETag`** (`W/"v1"`, RFC 9110 §8.8.3.3), still matched by `If-None-Match`, and **`206` responses are never compressed** (`status(206)` on a directly compressed body throws `std::logic_error`). See [Entity tags](guides/compression.md#entity-tags).
- **WebSocket `onClose` also reports the connections closed without closing handshake**, with `AbnormalClosure` (1006): lost, closed for their idle or close timeout, or by the server stopping. Each connection now ends with exactly one call of `onClose` or `onError`, so the state that the callbacks refer to must outlive the connections of the server. See [Callbacks](protocols/websocket.md#callbacks).

#### API

- **Reserved and invalid headers throw `std::invalid_argument` in every build**: `transfer-encoding`, `connection`, `date`, `trailer`, `te` and `upgrade` set by a handler (`headerAddLine()`, `header()`, `headerAppendValue()` of `HttpResponse` and `HttpResponseWriter`), reserved names given to the `HttpResponse` constructors taking concatenated headers, an upper-case `LowerAsciiKey` built at runtime, and forbidden trailers on `HttpResponse`, `HttpResponseWriter` and `HttpRequest` (`http::IsForbiddenTrailerHeader`). They were sent, or only asserted in debug builds. See [Reserved headers](guides/responses.md#reserved-headers) and [Trailers](guides/responses.md#trailers).
- **A streamed body must match its `contentLength()`**: a write beyond it sends the bytes still due and returns `false`, `end()` before it fails the writer (HTTP/1.1 connection closed, HTTP/2 stream reset), and `contentLength(0)` declares an empty body (it meant no length). See [Streaming responses](guides/streaming-responses.md#how-it-works).
- **Configuration validation**: `HttpServerConfig::validate()` rejects `location` and `content-encoding` in `globalHeaders` (use `HttpResponse::location()`), `TLSConfig::validate()` rejects `"h2c"` in the ALPN list (RFC 9113 §3.2, list `"h2"`), `WebSocketConfig::maxMessageSize` and `maxFrameSize` no longer accept `0` (use `std::numeric_limits<std::size_t>::max()`) nor `closeTimeout` a negative value, and `Http2Config::settingsTimeout`, `pingInterval` and `pingTimeout` must be positive (`std::chrono::milliseconds::max()` disables them). See [Configuration](protocols/websocket.md#configuration).
- **No more `0` meaning disabled or unlimited**: `max()` disables a timeout or lifts a limit, and `validate()` rejects 0 for `keepAliveTimeout` (server: at least 1 ms, 0 froze the event loop at the first maintenance finding an HTTP/2 stream in progress; `HttpClientConfig`: 0 disabled the expiry of pooled connections), `headerReadTimeout` and `bodyReadTimeout` (now `std::chrono::milliseconds::max()` by default, still disabled), `TLSConfig::handshakeTimeout`, `TLSConfig::maxConcurrentHandshakes` (no limit by default) and `WebSocketConfig::idleTimeout`. The `HttpClient` response cache is enabled by a non-zero `cache.maxEntries` (0 by default) instead of a non-zero `refreshPeriod` (now positive, 1 minute by default): `withCache()` sets both (1024 entries by default), and a configuration file enabling the cache with `refreshPeriod` alone must add `maxEntries`. Long timeouts are accepted: `headerReadTimeout` and `bodyReadTimeout` are no longer bounded to about 49.7 days, and a longer per-route `timeout()` (or `RouteGroup::withTimeout()`) is capped to `PathEntryConfig::kMaxRequestTimeout` instead of throwing `std::invalid_argument`. See [Server configuration](reference/server-configuration.md) and [Client configuration](reference/client-configuration.md).
- **API that depends on a feature**: the TLS configuration of `HttpServerConfig` (`tls`, `withTls*()`, `withoutTls()`, the `tls` section of configuration files) only exists with `AERONET_ENABLE_OPENSSL` (a build without OpenSSL served a configured TLS listener in clear text), `TLSConfig` and `TLSInfo` moving to the `aeronet_tls` library. The WebSocket compression API (`WebSocketConfig::deflateConfig`, `DeflateConfig`, `DeflateNegotiatedParams`, `SharedDeflateStreams`, `enableCompression()`, `hasCompression()`) only exists with `AERONET_ENABLE_ZLIB`. Include paths are unchanged. See [TLS](protocols/tls.md).
- **`WebSocketConfig` moved to `aeronet/websocket-config.hpp`** (still included by `websocket-handler.hpp`), and **`upgrade::IsSameOriginRequest()` takes whether the connection uses TLS**.
- **32-bit targets are rejected** at configure time and by a `static_assert`: they never worked in practice. See [64-bit targets only](getting-started/install.md#64-bit-targets-only).

### 1.8.0 Bug fixes

#### HTTP/1.1 parsing

- **Malformed input is answered at once**: a bare LF line ending (in the head, the lines of a chunked body, a `TRACE` request) and input that cannot start a request are answered `400` when received, instead of waiting for a timeout, or for `keepAliveTimeout` without any answer. See [Line endings](guides/requests.md#line-endings).
- **Request line**: empty lines before a request line are ignored (RFC 9112 §2.2); an over-long request target is `414` (was `431`); a method with an invalid character `400` (was `501` when received at once); a `Transfer-Encoding` not ending with `chunked` `400` (was `501`); `GET! /` is parsed right on big-endian platforms. See [Message parsing](reference/http-conformance.md#message-parsing).
- **An absolute-form target is routed by its path** (`GET http://example.com/a` was routed as that whole string) and its authority replaces `Host` (RFC 9112 §3.2.2); `HttpRequestView::authority()` returns it, also without HTTP/2. See [Request target forms](guides/requests.md#request-target-forms).
- **Header fields**: values with bytes above `0x7F` (`obs-text`) are accepted by the server and by `HttpClient`; an invalid `Content-Length` is rejected before any response from the head (`406`, expectation handler), which kept the connection open with the body served as a request; merging a repeated field whose first occurrence was empty no longer corrupts another field; repeated `Cookie` lines are joined with `; `. See [Header values](guides/requests.md#header-values) and [Header names](guides/requests.md#header-names).
- **Chunked bodies**: the decoding is linear in the body size (each read decoded it again from its first chunk: 15.7 s for 128 MiB in 16 KiB writes), a chunk exceeding `maxBodyBytes` is refused when announced, chunk-size and trailer lines are bounded by `maxHeaderBytes` (a line without end was buffered without limit), whitespace before a chunk extension is accepted, the trailer limit no longer depends on the reads, and repeated trailers no longer produce made-up ones (`accept: a` then `accept:   host:evil` gave a `host` trailer). See [Chunked bodies and trailers](guides/requests.md#chunked-bodies-and-trailers).
- **Expectations and `1xx`**: `100 Continue` is no longer sent when the final status is known from the head (`404`, `405`, CORS, request middleware), the status line of an `Interim` result has its mandatory space (`HTTP/1.1 103 `), `101` is refused as `Interim`, and no `1xx` response is sent to an HTTP/1.0 client (its `Expect` and `Upgrade` are ignored). See [Expectations](guides/requests.md#expectations) and [Expect](reference/http-conformance.md#expect).
- **`TRACE` echoes the request as received**, without `Authorization`, `Proxy-Authorization` and `Cookie` (RFC 9110 §9.3.8): it echoed the head decoded in place, with the body. See [TRACE](reference/http-conformance.md#trace).

#### HTTP/1.1 connections

- **Pipelining and close**: requests pipelined behind a slow reader are no longer answered `414` or `408` (the server stops reading while output waits, as for HTTP/2), a client half-closing right after its request receives the whole response, the lingering close also waits for the input still in the socket (RFC 9112 §9.6), and a `406` keeps the connection open. See [Connections and keep-alive](guides/server-lifecycle.md#connections-and-keep-alive) and [Lingering close](guides/server-lifecycle.md#lingering-close).
- **Slow readers**: a client that stops reading a file response, or any response with `enableKeepAlive` off, is closed once the response made no progress for `keepAliveTimeout` (new `aeronet.connections.closed_for_stalled_output` counter), and a client reading slowly but steadily is no longer closed in the middle of a response.
- **Timeouts**: those of a pipelined request count from its own start (they counted from the boot of the host, wrong after 49.7 days of uptime), and per-route timeouts close to `PathEntryConfig::kMaxRequestTimeout` no longer expire within milliseconds. See [Limits and timeouts](guides/requests.md#limits-and-timeouts) and [Per-route options](guides/routing.md#per-route-options).

#### Responses

- **Statuses without content**: a body set on a `204`, `205` or `304`, and the chunked framing of a streamed `204`, are no longer sent (the client read them as the next response), and error responses to `HEAD` (`400`, `406`...) no longer carry a body. See [Statuses without content](guides/responses.md#statuses-without-content) and [Status codes produced by the server](reference/http-conformance.md#status-codes-produced-by-the-server).
- **`HEAD` responses carry the headers of the `GET` ones**, compression included (`Content-Encoding`, `Vary`, `Content-Length`), and a streamed `HEAD` no longer announces `Content-Length: 0` when the `GET` is chunked. See [HEAD requests](guides/compression.md#head-requests).
- **`405` responses have the `Allow` header** that RFC 9110 §15.5.6 requires, and **`OPTIONS *` lists the methods of the server** (new `Router::supportedMethods()`): it listed a route named `*`, and `TRACE` while disabled. See [OPTIONS](reference/http-conformance.md#options).
- **The `Date` of a response is the second of its generation**, for about 1 ns per response: it was refreshed once per event loop iteration (3.5 s old behind a slow handler). See [Reserved headers](guides/responses.md#reserved-headers).
- **HTTP/1.0 clients**: streaming routes serve them (they were answered `404`), with a body delimited by the close without `contentLength()`, and they no longer receive a chunked response with trailers (the trailers are dropped). See [Streaming responses](guides/streaming-responses.md#how-it-works).
- **Redirects stay on the site**: the trailing slash and static directory redirects of a path starting with `//` gave `Location: //evil.example` (now `/.//evil.example`), and the directory redirect re-encodes its path (a name holding CR LF could inject header lines). See [Trailing slashes](guides/routing.md#trailing-slashes).
- **`bodyAppend()` on a `HEAD` response made with `makeResponse(body)` no longer corrupts memory.**

#### Compression

- **`maxExpansionRatio` is enforced while decoding**, by the server and `HttpClient` (buffered or streamed to a sink): a few MiB of gzip inflated gigabytes before the `413`. **`maxCompressedBytes` refuses a body from its declared length** or from the HTTP/2 DATA frame exceeding it, instead of once received up to `maxBodyBytes`. See [Request body decompression](guides/compression.md#request-body-decompression).
- **Compressed streamed responses**: a `contentLength(n)` response compressed on the fly kept `Content-Length: n` with fewer bytes; the compression is now decided from the declared length, and a compressed body is chunked. `Accept-Encoding` is listed once in `Vary`. See [Compression](guides/compression.md#streaming-responses).
- **br and zstd no longer truncate large incompressible bodies**: streamed after about 1.5 MiB with br, single writes above 256 KiB with zstd, and direct compression of a few MiB lost the rest silently in release builds. **Bodies of a few compression chunks are compressed**: the worst case of the next chunk stopped the compression early. See [Direct compression](guides/compression.md#direct-compression).
- **A compressed body with trailers keeps a valid chunked framing**: a stray byte came before the first chunk with the default `useLeadingZeroesInContentLength`. See [Trailers](guides/responses.md#trailers).
- **Negotiation follows RFC 9110**: empty `Content-Encoding` elements are ignored and `x-gzip` is `gzip`, a `415` lists the decoded codings in `Accept-Encoding`, quality values follow the grammar (`q=2`, `q=1e9`, `q=nan` count as 0), a duplicate `*` keeps its highest weight, an invalid weight on `identity` or `*` no longer answers `406`, and a `;` in a quoted parameter no longer splits it (multipart `filename="report; v2.txt"`). See [Negotiation](guides/compression.md#negotiation) and [Forms and file uploads](guides/requests.md#forms-and-file-uploads).
- **`contentTypeAllowList` compares media types without their parameters** (`text/html` did not match `text/html; charset=utf-8`), and applies to streamed responses too. See [Compression](guides/compression.md).

#### Static files

- **A file that shrinks while sent no longer freezes the server** (100% CPU over user-space TLS until it grew back): the connection is closed after the bytes sent. See [Transfers](guides/static-files.md#transfers).
- **A file replaced by a FIFO cannot block the event loop**: `File` opens read-only files with `O_NONBLOCK`, and the handler checks the new `File::isRegularFile()`.
- **Conditional requests**: dates are compared at the second (a client sending back `Last-Modified` got `200`), `If-None-Match` uses the weak comparison so that compressed files revalidate with `304` (carrying their weak `ETag` and `Vary`, see the new `HttpRequestView::bodyCompressionOutlook()`), preconditions are evaluated in the order of RFC 9110 §13.2.2, the obsolete HTTP date formats are accepted and misspelled dates (`Npl`, `GNJ`) rejected. See [Caching and conditional requests](guides/static-files.md#caching-and-conditional-requests).
- **Ranges**: positions beyond 64 bits stand for the end of the file (they were `416`), and a suffix range of an empty file is `200`. See [Ranges](guides/static-files.md#ranges).
- **Directory listings**: entries beyond `maxEntriesToList` cost a comparison (12,000 files with a limit of 3,000 took 7 s in a Debug build), the HTML is written in one allocation, and symbolic links answered `404` are no longer listed. See [Mapping paths to files](guides/static-files.md#mapping-paths-to-files).

#### CORS

- **`allowAnyOrigin()` with credentials no longer grants the `null` origin**, which any site can make a browser send, nor stops the server on an `Origin` with non-ASCII bytes over HTTP/2 (the event loop thread exited): both are answered `403`. See [CORS](guides/cors.md#configuration).
- **Responses to requests without `Origin` vary on `Origin`**, so that a cache cannot serve them to CORS requests, and **`allowAnyRequestHeaders()` covers `authorization` and credentialed requests**. See [CORS](guides/cors.md#behavior).

#### Async handlers

- **Crashes**: a per-route `timeout()` expiring during deferred work let the lingering close reuse the buffer of the request (memory corruption in Release), and one expiring while the HTTP/2 output was full dereferenced a null pointer; `readBody()` before the body is received now throws `std::logic_error` instead of crashing. See [Time out slow requests](guides/async-handlers.md#time-out-slow-requests).
- **Request data**: the request no longer changes while deferred work reads it (a body received meanwhile was decompressed in place), a suspended handler no longer reads the compressed upload of another client from a shared buffer, and the captures of a work function are destroyed before its handler resumes. See [What a work function may do](guides/async-handlers.md#what-a-work-function-may-do) and [Process an upload off the event loop](guides/async-handlers.md#process-an-upload-off-the-event-loop).
- **Body access and timeouts**: a compressed body can be read in chunks, `body()` called before the body arrives no longer loses it, the body of an answered request is no longer decompressed, `keepAliveTimeout` closes stalled uploads, `bodyReadTimeout` no longer interrupts deferred work, and a completed zerocopy send no longer closes the connection during deferred work. See [Reading the request body](guides/async-handlers.md#reading-the-request-body).
- **HTTP/2**: `maxConcurrentStreams` bounds the deferred work of reset streams (a client could start any number of threads), the `408` of a route timeout is sent at once, without a forbidden `RST_STREAM` after it, and counted as a completed request, and the last deferred response after a `GOAWAY` closes the connection right away. A handler throwing before its body arrives goes through the pipeline. See [HTTP/2 async handlers](guides/async-handlers.md#http2).

#### HTTP/2 and HPACK

- **Connection lifetime**: `settingsTimeout`, `pingInterval`, `pingTimeout`, `maxStreamsPerConnection` and `enablePriority` were documented but ignored (a connection with open streams escaped `keepAliveTimeout` after its client disappeared); a drain sends a graceful `GOAWAY` and serves the open streams; a graceful `GOAWAY` of the client no longer drops its open streams; a second `GOAWAY` tells the cause of a later connection error. See [Connection lifetime](protocols/http2.md#connection-lifetime).
- **HPACK state kept in sync**: header blocks on closed, half-closed, forgotten, self-dependent or post-`GOAWAY` streams are decoded before being ignored or reset, a reference to an entry with an empty name only resets its stream, and the encoder signals the smallest of several table size changes (RFC 7541 §4.2). Each of these failed later requests of the connection with `COMPRESSION_ERROR` (server and `HttpClient`).
- **HPACK bounds**: a crafted string length no longer makes the decoder read past the block (crash), integers beyond 32 bits and misplaced table size updates are `COMPRESSION_ERROR`, the decoder table is bounded by `headerTableSize` (a peer could raise it to 4 GiB) and the encoder one by the smaller of both settings, Huffman strings of fields beyond `maxHeaderListSize` are released, and the encoder keeps the static index of a name. See [Flow control and memory](protocols/http2.md#flow-control-and-memory).
- **Abuse bounds**: the new `Http2Config::maxResetStreams` closes connections resetting most of their streams (Rapid Reset, CVE-2023-44487, and MadeYouReset, CVE-2025-8671), more than 16 empty `DATA`, `HEADERS` or `CONTINUATION` frames in a row close the connection (CVE-2019-9518), the new `streamIdleTimeout` closes streams waiting for their client (`aeronet.http2.streams.closed_for_idle_timeout`), priority dependencies no longer form cycles, and the `PRIORITY` frame limit counts since the last stream opened (it closed long-lived connections). See [Request validation](protocols/http2.md#request-validation).
- **Stream errors instead of connection errors**: a stream above `maxConcurrentStreams` is refused with `REFUSED_STREAM`, a window made negative by `SETTINGS_INITIAL_WINDOW_SIZE` waits for credit, a larger setting no longer applies to closed streams, a DATA frame refused by its stream window gives back its connection credit, and late frames on a forgotten stream are ignored.
- **Conformance**: connection-specific fields make a request malformed (a smuggling path through `transfer-encoding` to an HTTP/1.1 backend) and are dropped from responses; the first frame must be `SETTINGS`; an invalid preface is rejected at its first byte; frames on idle streams are connection errors; no `RST_STREAM` is sent on a closed stream; a request answered before its end is reset with `NO_ERROR` (new `aeronet.http2.streams.rejected_malformed` counter); window settings below 65,535 bytes apply once acknowledged or are rejected by `validate()`; a header block on a CONNECT stream being resolved resets it. See [Request validation](protocols/http2.md#request-validation) and [CONNECT over HTTP/2](protocols/connect.md#connect-over-http2).
- **A file failing at the start of a response resets its stream** (the client waited forever), and **errors caused by clients are logged at debug level** (any client could fill the logs).

#### WebSocket

- **permessage-deflate with context takeover** (the default): a message that compression did not shrink (random or already compressed data) was sent uncompressed although it was in the compression stream, so the next compressed message failed to decompress on the peer (`1007`) or decompressed to wrong bytes. It is now sent compressed. See [Sending messages](protocols/websocket.md#sending-messages).
- **`WebSocketEndpoint::WithCallbacks()`, `WithConfigAndCallbacks()`, `WithProtocolsAndCallbacks()` and `WithFullConfig()` gave their callbacks to the first connection only**: the next connections of the endpoint got none. See [Defining endpoints](protocols/websocket.md#defining-endpoints).
- **The buffers of fragmented and compressed messages no longer stay allocated**: above 64 KiB, they are freed once the message is delivered. A compression bomb of a few kilobytes left `maxMessageSize` bytes (64 MiB by default) allocated per connection. See [Configuration](protocols/websocket.md#configuration).
- **An exception escaping a WebSocket callback no longer stops the event loop of the server**: it is logged, and the connection fails with `InternalError` (1011). An exception escaping the input processing of an HTTP/2 connection (an allocation failure) now closes the connection too. See [Callbacks](protocols/websocket.md#callbacks).
- **Nothing the peer sends after its Close frame or after a failure is processed** (RFC 6455 §7.1.7): in client mode, a message following the Close frame was delivered, and a second Close frame called `onClose` again.
- **A `Sec-WebSocket-Key` with a byte above `0x7F` no longer reads out of bounds** (an abort with `_GLIBCXX_ASSERTIONS`, the default on Fedora and RHEL): it is answered `400`, like a key with a misplaced `=`. See [Upgrade handshake](protocols/websocket.md#upgrade-handshake).
- **Handshake**: the same-origin check compares the scheme (an `http://host` page could open the TLS endpoint of `host`), an unsupported version is answered with `Sec-WebSocket-Version: 13`, the subprotocol is answered with the client spelling, `Upgrade: websocket, foo` upgrades, and a `GET` without upgrade on an endpoint without `GET` handler is `426`. See [Defining endpoints](protocols/websocket.md#defining-endpoints).
- **Close frames**: codes that may not be sent are answered `1002`, a reason that is not UTF-8 `1007`, `sendClose()` with such a code sends no status code, long reasons are truncated on a code point, and a drain closes connections with `1001` Going Away. See [Closing connections](protocols/websocket.md#closing-connections).
- **permessage-deflate**: RSV1 on a continuation frame fails the connection, messages after one ending with a final DEFLATE block are no longer empty, truncated data fails with `1007`, a message decompressing beyond `maxMessageSize` with `1009`, and invalid offers are declined (RFC 7692 §7). See [Compression](protocols/websocket.md#compression-permessage-deflate).
- **Client mode masking keys are unpredictable** (`CreateClientWebSocketHandler()`, secure system generator, RFC 6455 §5.3), invalid UTF-8 in a fragmented text message fails at its fragment, and `sendText()` asserts valid UTF-8 in debug builds. See [Callbacks](protocols/websocket.md#callbacks).

#### HTTP client

- **HTTP/1.1 responses**: a duplicate `Content-Length` and non 3-digit statuses are rejected, a `Transfer-Encoding` not ending with `chunked` is delimited by the close, a reason phrase with control bytes fails with `malformedResponse` and a non-ASCII one is ignored (both threw `std::invalid_argument`), the version name is checked, and bytes after the end of a response close the connection instead of being taken for the next response. See [Connections](modules/http-client.md#connections) and [Responses](modules/http-client.md#responses).
- **Early responses**: a final response received while the body is sent stops the upload and is returned (it was a `writeError` or a `timeout`), and `Expect: 100-continue` is supported (new `expectContinueTimeout`, a `417` repeats the request without it). See [Early responses and `Expect: 100-continue`](modules/http-client.md#early-responses-and-expect-100-continue).
- **HTTP/2**: connection-specific headers are no longer sent, responses carrying them fail with `malformedResponse`, repeated `Set-Cookie` fields stay separate, and no false error is logged when a sink stops a small response. See [HTTP/2](modules/http-client.md#http2) and [Streaming response bodies](modules/http-client.md#streaming-response-bodies).
- **A host longer than 65535 bytes is an invalid URL** (`std::invalid_argument`, or `invalidUrl` for a redirect, instead of `std::overflow_error`); the new `HttpRequest::IsValidUrl()` checks a URL without exception. See [Requests](modules/http-client.md#requests).
- **Request targets of redirects and of URLs without a path**: a `Location` holding only a query (`?page=2`) replaces the query of the request (it was appended: `/list?page=1?page=2`), a URL without a path (`http://host?q=1`) is requested as `/?q=1` (the request line carried `?q=1`, which servers reject), and a relative redirect through a cleartext proxy keeps the absolute URL as target (the proxy got the path alone). See [Redirects](modules/http-client.md#redirects).
- **A redirected cached request is stored under the key of the request of the caller**: it was never served from the cache, and a redirect to a longer URL read freed memory. See [Response cache](modules/http-client.md#response-cache).

#### TLS, server and platforms

- **HTTP/1.1 over TLS sets `TCP_NODELAY` under the default `Auto`**, as documented (only HTTP/2 did): the first response of a connection took 43.6 ms instead of 2.5 ms, and 16 to 64 KiB responses were served at 2.3k requests/s instead of 60-86k. See [Settings](operations/performance.md#settings).
- **A failing `epoll_wait()` stops the server** instead of spinning at 100% CPU, an interrupted `sendfile()` is retried at once instead of stalling its response, and the exception of a failed event-loop registration carries its own error code. See [Stopping](guides/server-lifecycle.md#stopping).
- **The lines of the built-in logger no longer interleave** between threads. See [Logging](operations/observability.md#logging).
- **The total `ServerStats` of `MultiHttpServer::stats()` counts kTLS.** See [Observability](protocols/tls.md#observability).

#### Build and packaging

- **An installed aeronet works for its consumers**: the headers of a downloaded amc are installed with it, and `find_package(aeronet)` finds zlib for a build with `AERONET_ENABLE_ZLIBNG=OFF`, opentelemetry-cpp, brotli and a zlib-ng installing only `zlib-ng::zlib`, creating the `amc::amc` stub before loading the targets. See [Install](getting-started/install.md#install).
- **The vcpkg port applies its features**: all of them were configured `OFF`, and the features without a dependency keep their default (`ON`).
- **opentelemetry-cpp ABI v1 is supported**, the default of vcpkg, Conan and system packages (`gauge()` and `timing()` are exported as up-down counters there), and the Conan package builds with the opentelemetry-cpp, the toolchain (Debug builds) and the zstd of Conan. See [opentelemetry-cpp ABI](operations/observability.md#opentelemetry-cpp-abi).
- **The dependencies downloaded at configure time are always static libraries** (brotli forced `BUILD_SHARED_LIBS` on).
- **`fullVersionStringView()` / `fullVersionWithRuntime()` report the OpenTelemetry version and the compile-time features.**

### 1.8.0 Improvements

#### New features

- **WebSocket client**: `WebSocketClient` opens `ws://` and `wss://` connections and serves any number of them from the thread calling `poll()`, with `permessage-deflate`, automatic pings, idle and close timeouts, and callbacks receiving each message without copy. The handshake goes through an `HttpClient` (timeouts, TLS, cookies, forward proxy through a `CONNECT` tunnel), and `HttpClient::upgrade()` performs the handshake of any protocol switched to by a `101`. `AERONET_ENABLE_WEBSOCKET` defaults to `ON` without the server too, and the `aeronet-websocket-client` example is a command-line client. See [WebSocket client](protocols/websocket-client.md).
- **Cookies in `HttpClient`** (`HttpClientConfig::withCookies()`, off by default): the cookies set by responses are sent back with the matching requests, as browsers do (RFC 6265bis: `Domain`, `Path`, expiry, `Secure`, prefixes, `SameSite` along redirects), and `HttpClient::cookieJar()` manages them. See [Cookies](modules/http-client.md#cookies).
- **HTML parsing and CSS selectors** (`AERONET_ENABLE_HTML`, lexbor; vcpkg feature `html`, Conan option `with_html`): `HtmlDocument` parses a page as browsers do (encoding sniffing included), queries it with CSS selectors (`querySelector()`, `querySelectorAll()`, `forEach()`, compiled `CssSelector`), reads elements (`textContent()`, `innerHtml()`, `assignedJson()` for the JSON state embedded by server-rendered pages) and modifies the tree as the DOM does. See [HTML parsing](modules/html.md).
- **JavaScript pages** (`AERONET_ENABLE_JS`, QuickJS-ng; vcpkg feature `js`, not in the Conan package): `WebClient::navigate()` loads a page and runs its scripts in the order of a browser, with those that scripts insert (webpack chunks), over DOM bindings with events, timers on a virtual clock, `fetch()` and `XMLHttpRequest` (CORS, credentials and `SameSite` cookies of a browser), `URL`, script navigations, `history`, storage, `document.cookie`, the observers, media queries and `Intl` (en-US) of single-page apps (a React production build renders), and returns a `WebPage` once a selector matches, a load state is reached or a timeout runs out, its resources bounded by `WebClientConfig`. As the Local Network Access of browsers, neither a page nor a redirect makes the client reach a server less public than its own (the host, its local network), checked on the resolved addresses before connecting, `WebClientConfig::addressSpace` can keep `navigate()` itself on the Internet, and `allowLessPublicRedirects` restores the redirect rule of browsers for intranet single sign-on. The bytecode of the scripts is kept for the next pages of a client. New `HttpClient::requestUpdating()` and `CookieJar::setDocumentCookie()` / `documentCookie()`. See [JavaScript pages](modules/web.md).
- **Protocol Buffers bodies** (`AERONET_ENABLE_PROTOBUF`, header-only; vcpkg feature `protobuf`, Conan option `with_protobuf`): `bodyProto()` serializes a message into a response or a client request, `bodyAsProto<T>()` parses a request or a response body, binary or ProtoJSON according to its `Content-Type` (RFC 9996), and `makeProtoResponse()` negotiates the encoding with `Accept`. `AERONET_PROTOBUF_RUNTIME` selects the `full` or `lite` runtime. See [Protocol Buffers](modules/protobuf.md).
- **Strict SNI** (`TLSConfig::sniMustMatch`, off by default): the handshake fails for a client sending an unknown name or none, so that a scanner connecting to the IP address no longer gets the main certificate. See [Strict SNI](protocols/tls.md#strict-sni).
- **New options and API**: `HttpServerConfig::socketSendBufferBytes` (`SO_SNDBUF` of accepted connections), `CompressionConfig::addVaryAcceptEncodingToUncompressed` (`Vary: Accept-Encoding` on uncompressed variants too, as nginx `gzip_vary`), `headerAppendValueIfAbsent()` on `HttpResponse` and `HttpRequest`, and the `Router::supportedMethods()`, `HttpRequest::IsValidUrl()`, `HttpRequestView::bodyCompressionOutlook()` and `File::isRegularFile()` above. See [Server configuration](reference/server-configuration.md#event-loop-buffering-and-performance-controls), [Vary](guides/compression.md#vary) and [Status and headers](guides/responses.md#status-and-headers).

#### Performance

- **TLS**: a read that drains the socket no longer tries it again, saving one system call per request (5.6% more requests per second over user-space TLS, 6.9% over kTLS, 3.8% for HTTP/2 over TLS); writes no longer stop after the first 16 KiB record (an upload of 256 KiB echoed takes 9 event-loop waits instead of 21); and idle connections release the buffers of OpenSSL after `TLSConfig::idleBufferReleaseDelay` (1 s): 100,000 idle HTTP/1.1 connections take 1.6 GB of heap instead of 4.9 GB (`ServerStats::tlsIdleBufferReleases`). See [TLS record I/O](protocols/tls.md#tls-record-io) and [Memory of idle connections](protocols/tls.md#memory-of-idle-connections).
- **Fewer system calls**: `TCP_NODELAY` is set once on the listening socket (Linux), and `HttpClient` waits for the response before reading (4 system calls per pooled plaintext request instead of 5). See [Connections](modules/http-client.md#connections).
- **Async handlers**: 3 allocations per request completing right away instead of 5, no default constructible `RequestTask<T>` result needed, and handlers completing right away on pipelined requests no longer take a quadratic time. See [Performance](guides/async-handlers.md#performance).
- **HTTP/2 header blocks**: HPACK Huffman decoding up to 7 symbols per 8-byte read and encoding 4 symbols at a time (2 to 3 times faster), field values validated 8 bytes at a time, non-indexed response fields encoded without scanning the dynamic table, fields that are not in the table looked up by a fingerprint (a response with ten varying fields encoded 3 times faster), and fields already accepted from the peer not checked again: 8 to 10% fewer server cycles per request, 16 to 23% with 12 to 24 repeated fields. See [HTTP/2](protocols/http2.md).
- **HTTP/2 output**: DATA payloads up to 1 KiB are copied beside their frame header, so that a batch of small responses is written at once (7% less server CPU), the output buffer is shrunk once written, and DATA frames take 6 to 8% fewer cycles. See [Flow control and memory](protocols/http2.md#flow-control-and-memory).

### 1.8.0 Others

- **Bumped zlib from 1.3.2 to 1.4** (downloaded with `AERONET_ENABLE_ZLIBNG=OFF`; its security fixes concern functions aeronet does not use). The Conan recipe keeps `zlib/[~1.3]` until ConanCenter provides 1.4.
- **Bumped amc from 2.7.0 to 2.8.1**: an allocation failure in `shrink_to_fit()` (periodic trimming, `HttpClient::releaseUnusedMemory()`) now throws `std::bad_alloc` instead of terminating the process.

## [1.7.0] - 2026-10-26

### 1.7.0 Highlights

The most important changes. Upgrade especially if one of these applies to you:

- **Cross-site WebSocket hijacking**: upgrades bypassed request middleware and CORS and accepted any `Origin`. They now go through the middleware and check the origin.
- **Secrets served by the static file handler**: `/.env`, `/.git/config` were served, and symbolic links were followed outside of the root directory.
- **HTTP/2 memory exhaustion**: a 4.5 KB HPACK header block could make the decoder allocate 2.9 GB, and request bodies were only bounded once fully received.
- **Credentials leaked by the HTTP client**: redirects to another origin kept `Authorization` / `Cookie`, and HTTPS to HTTP redirects were followed.
- **Server stalls and crashes**: CONNECT name resolution and OpenTelemetry span export blocked the event loop, a connection arriving while the server stops could crash it, and a write on a connection reset by the peer could raise `SIGPIPE` and terminate the process.
- **TLS**: intermediate certificates were not sent, mTLS sessions never resumed, and an OpenSSL error left by a handler (a forged JWT signature, for instance) could close healthy connections.
- **Use-after-free and data races**: HPACK table eviction, async handlers and `deferWork()` outliving their connection, router or buffers, `MSG_ZEROCOPY` buffers, default OCSP response with SNI certificates, and many races on `stats()`, callbacks and the server lifecycle. The whole test suite now runs clean under ThreadSanitizer (`AERONET_ENABLE_TSAN`).
- **HTTP/1.1 requests received in several reads** were parsed again on each read (`400` with duplicate headers or a query string).
- **Performance**: a large round of HTTP/1.1, HTTP/2, TLS, WebSocket and telemetry work (see Improvements > Performance), and much smaller `HttpResponse` / `HttpRequest` objects.

### 1.7.0 Breaking changes

#### Behavior

- **Static files: hidden files and links leaving the root are no longer served**: with `showHiddenFiles` off (the default), a path segment starting with a dot (`/.env`, `/.git/config`, except `.well-known`), a link resolving outside of the root or to a hidden path (Windows short names included), and anything but regular files and directories are answered `404`. See [Static files](guides/static-files.md#mapping-paths-to-files).
- **WebSocket upgrades go through the request middleware and check the `Origin`**: the CORS policy of the route decides, otherwise only same-origin browsers and clients sending no `Origin` may connect (`403`). Behind a reverse proxy rewriting `Host`, give such routes a CORS policy or forward the browser `Host`. A factory returning `nullptr` refuses the upgrade (`403`). See [WebSocket upgrade handshake](protocols/websocket.md#upgrade-handshake).
- **WebSocket connections have their own idle timeout**: the new `WebSocketConfig::idleTimeout` (60 s, with a ping after half of it, `autoPing`) instead of `keepAliveTimeout`. See [Idle connections](protocols/websocket.md#idle-connections).
- **The `500` answering a throwing handler has no body**: the exception message is only logged.
- **HTTP client redirects keep credentials within their origin and never downgrade to cleartext**, and over HTTP/2 the client no longer adds its `globalHeaders` to each request: a request carries its own headers, as over HTTP/1.1. See [Redirects](modules/http-client.md#redirects).
- **`Accept-Encoding: *;q=0` without `identity` is answered `406`** (RFC 9110 §12.5.3).
- **HTTP/2 requests whose `content-length` does not match their body are reset** with `PROTOCOL_ERROR` (RFC 9113 §8.1.1). See [Request validation](protocols/http2.md#request-validation).
- **`maxDecompressedBytes` bounds the whole decoded body** (chunked and streamed decoding included), and exceeding it is answered `413` (was `400`).
- **Received `Content-Encoding` values are lower-cased** (RFC 9110 §8.4.1). See [Request body decompression](guides/compression.md#request-body-decompression).
- **HTTP/2 streams start with a 1 MiB receive window** (`Http2Config::initialWindowSize`, was 65,535 bytes, about 64 KiB per round trip), for the server and `HttpClient`. See [HTTP/2 configuration](protocols/http2.md#configuration).
- **`maxPerEventReadBytes` is a budget of bytes read plus written per event, and `minReadChunkBytes` the initial read size** of a connection (adaptive afterwards): a client sending small requests for large responses kept its connection served while the others waited.
- **`HttpClient` with `keepAlive = false` sends `Connection: close`** (RFC 9112 §9.6), so that the server closes first.

#### TLS

- **TLS handshakes time out after 10 seconds by default** (`TLSConfig::handshakeTimeout`, 0 to disable). See [Handshake protection](protocols/tls.md#handshake-protection).
- **TLS sessions only resume with session tickets** (the OpenSSL server-side cache is off), accepted for `sessionTickets.lifetime` (24 hours, was 2). See [Session tickets](protocols/tls.md#session-tickets).
- **Stricter TLS configuration validation**: a certificate and key given both as files and in memory, a cipher list with a policy other than `Default`, a maximum version below the minimum, an SNI pattern mapped twice, and `"h2"` in ALPN without HTTP/2 are rejected. `withTlsCertKey()` clears the in-memory credentials.
- **`alpnMustMatch` also rejects a client offering no ALPN protocol** (`no_application_protocol`, counted in `tlsAlpnStrictMismatches`).
- **Malformed TLS credentials throw `std::runtime_error` with the OpenSSL reason** (was `std::bad_alloc`), and an encrypted private key is rejected instead of OpenSSL prompting for its passphrase on the terminal.
- **The `Legacy` cipher policy extends `Compatibility`**: it allowed four RSA CBC ciphers only. Each policy extends the previous one for ECDSA and RSA certificates, with the same TLS 1.3 suites and AEAD ciphers first.
- **HTTP client: an unset `tlsMinVersion` means TLS 1.2**, as on the server.

#### API

- **DogStatsD metrics and `TelemetryContext` counters are sent at `flush()`**: the server and `HttpClient` flush their own, a client or context of your own must call `flush()` periodically. See [Batching and flush](operations/observability.md#batching-and-flush).
- **`RequestTask` coroutines only accept the awaitables they support** (`RequestTaskAwaitable` concept: aeronet awaitables, `std::suspend_always`, `std::suspend_never`); a custom awaitable can declare `using AeronetAwaitableTag = void;`.
- **HTTP client: `responseTooLarge` and `connectionClosed` errors** for oversized responses and HTTP/1.1 responses cut short, `malformedResponse` being left to responses that cannot be parsed or decoded. See [Error model](modules/http-client.md#error-model).
- **`HttpServerConfig::connectAllowlist` is a public field** (was `connectAllowlist()`), and **`MultipartFormData::Part::filename` and `contentType` are `std::string_view`** (`Part::hasFilename()` tells an empty filename apart).
- **Glaze metadata of the configuration objects moved to companion headers** (`<aeronet/tls-config-glaze.hpp>`, `<aeronet/http-server-config-glaze.hpp>`...): code serializing configuration objects with Glaze itself must include `<aeronet/aeronet-config.hpp>` or `<aeronet/http-client-config-glaze.hpp>`. Translation units compiling Glaze went from 149 to 24. See [Serialize configuration objects yourself](modules/json-and-config-files.md#serialize-configuration-objects-yourself).
- **Module and header reorganization** (include paths unchanged): `ConnectionState`, the request pipeline and `Http2ProtocolHandler` moved to `aeronet_server`, `aeronet_http2` is the HTTP/2 protocol core, and a target linking `aeronet_http` or `aeronet_http2` without `aeronet_server` must link `aeronet_tls` / `aeronet_websocket` itself. `WebSocketEndpoint` needs `<aeronet/websocket-endpoint.hpp>`, and the umbrella headers no longer include the HTTP/2 internals.
- **Low-level API changes**: `Transport(fd)`, `PlainTransport(fd)` and `TlsTransport(ssl)` constructors without zerocopy state (the server decides each `MSG_ZEROCOPY` write); `HpackDecoder::decode()` returns `DecodedHeaders` and enforces the header list size limit; `FrameHeader`, `DataFrame` and `HeadersFrame` accessors; `IProtocolHandler::processInput()` without `ConnectionState&`, new `onSweep()` and `hasRequestsInProgress()`.

### 1.7.0 Bug fixes

#### HTTP/1.1 parsing, pipelining and connection close

- **Requests received in several reads are parsed once**: the head was parsed again at each read although parsing modifies it in place, answering `400` with duplicate headers or a query string (and decoding the target twice before 1.6.0). Such requests are also counted once and get a single `100 Continue`.
- **A final response to an expectation no longer stalls the pipelined requests, nor reads a body as a request**: a request announcing a body closes the connection. See [Expect](reference/http-conformance.md#expect).
- **Input left behind a blocked output is always served** (it waited for more client data or the keep-alive timeout when the output drained elsewhere).
- **A response closing a connection before the end of the request reaches the client**: the server shuts down the write side and discards input until the client closes (RFC 9112 §9.6), for at most the new `lingeringCloseTimeout` (5 s). See [Lingering close](guides/server-lifecycle.md#lingering-close).
- **Streamed HTTP/1.1 responses announce every close** with `Connection: close`, like buffered ones.
- **A streaming handler throwing is answered `500`, or its response is signaled incomplete** (HTTP/1.1 connection closed, HTTP/2 stream reset): over HTTP/2 the response was completed with a `200`, over HTTP/1.1 a `500` was written after the bytes sent. Same for a streamed compression failing after the head.

#### CONNECT

- **Target names are resolved off the event loop** (`getaddrinfo` blocked every connection), at most 4 at a time and 256 waiting per server thread (`503` beyond, `504` after `keepAliveTimeout`). See [Name resolution](protocols/connect.md#name-resolution).
- **The `200` of an HTTP/1.1 CONNECT has no content** (its body broke TLS handshakes through the tunnel), **CONNECT to an IPv6 address works**, and **tunnels no longer buffer without limit** (a side stops being read while `maxOutboundBufferBytes` wait for the other). See [Backpressure](protocols/connect.md#backpressure).

#### HTTP/2

- **HPACK: no read of freed memory when a new field takes the name of the entry it evicts** (since 1.5.0), and **a header block can no longer exhaust memory**: `maxHeaderListSize` is enforced while decoding and repeated fields joined once (a 4.5 KB block allocated 2.9 GB, now 17 KB).
- **Request bodies are bounded while received** (`413` as soon as `maxBodyBytes` or the declared `content-length` exceeds it), and **file transfers are bounded in memory** (read ahead only under half of `maxOutboundBufferBytes`: a 1 GiB window loaded the whole file). See [Flow control and memory](protocols/http2.md#flow-control-and-memory).
- **Repeated request fields are combined as over HTTP/1.1** (two values were joined by the letter `O`). See [Header names](guides/requests.md#header-names).
- **Credentials are never indexed (CRIME)**: `authorization`, `proxy-authorization` and `cookie` values shorter than 20 bytes are sent as never-indexed literals (RFC 7541 §7.1.3). See [HTTP/2](protocols/http2.md#protocol-support).
- **HTTP/2 requests go through the request pipeline of HTTP/1.1**: cleartext HTTP/2 skipped the HTTPS redirect, a handler ran before the CORS `403`, trailing slash redirects were `404`, middleware lacked path parameters and metrics, and streaming and error responses skipped the response middleware and CORS headers. See [Handlers](protocols/http2.md#handlers).
- **Frames received on a reset stream are ignored** (RFC 9113 §5.1), their header blocks decoded and their connection credit given back (each DATA got a `RST_STREAM`, and the HPACK state went out of sync), server and client.
- **`readBody()` / `readBodyAsync()` work**, and **HTTP/2 requests know their connection**: `clientAddress()` and the TLS getters dereferenced a null pointer, crashing the server with a metrics callback or an access log.

#### Async handlers and `deferWork()`

- **Request data stays valid while a handler is suspended** (query parameters, `Content-Length` body, path parameter names), the next bytes being left in the socket until it completes.
- **Router updates no longer free the state of suspended handlers**, which are shared with the requests running them.
- **The coroutine frame and the request outlive a closed connection or stream until `deferWork()` completes**: a work function capturing them by reference used freed memory. Destroying the server waits for the work still running, and `deferWork()` no longer terminates the process when no thread can be created.
- **An async handler throwing when called no longer stops the server** (`500` through the response middleware).
- **A response sent before the request body was received no longer loops** (tens of thousands of `401` per second), and **requests pipelined behind an async handler are served right away**.

#### TLS

- **Intermediate certificates are sent** (only the first certificate of a file or PEM was loaded), by the server and by the `HttpClient` mTLS client certificate, and trusted client certificate bundles are fully loaded. See [Certificates](protocols/tls.md#minimal-https-listener).
- **mTLS sessions resume**: no session id context was set, failing every reconnection. See [Mutual TLS](protocols/tls.md#mutual-tls).
- **An OpenSSL error left by other code no longer closes healthy connections** (a JWT check of a forged RS256 signature, which any client can send).
- **A handshake larger than the socket send buffer completes** (the client waited for the rest of a long chain until a timeout).
- **Robustness of the OpenSSL integration**: no exception escapes the ticket and key log callbacks, no crash on allocation failure while setting up a connection with OpenSSL 3.2+, and aeronet builds against OpenSSL 4.
- **Hot reload**: TLS files rotated at the same path (in place, atomic rename, Kubernetes secret `..data` swap) are reloaded by `postConfigUpdate()`, a standalone server keeps its ticket keys across a rebuild, and the default OCSP response is no longer read from freed memory with SNI certificates. See [TLS hot reload](protocols/tls.md#hot-reload-and-failure-behavior).
- **In-memory private keys leave no copy in freed memory** (`TLSConfig` strings, client key PEM, proxy URL, static ticket keys).
- **Smaller fixes**: the handshake rate limiter after a long idle period (32-bit refill wraparound), SNI names with a trailing dot, `handshake_error` for transport errors, `tlsAlpnStrictMismatches` kept across rebuilds, a spurious ticket key warning; HTTP client: no SNI for IP addresses, Windows trusted roots loaded from the `ROOT` store.

#### WebSocket

- **`permessage-deflate` interoperates with browsers**: raw DEFLATE (RFC 7692) instead of the zlib wrapper, negotiated window bits applied, the sync flush marker always inflated with context takeover (the next message failed with `1007`), and `maxMessageSize` applied to the decompressed size. See [Compression](protocols/websocket.md#compression-permessage-deflate).
- **Echoing a compressed message no longer reads freed memory**, and **endpoint factories keep their callbacks when compression is negotiated** (new `WebSocketHandler::enableCompression()`).
- **Frames split across reads are delivered once and intact**, and frames received with the upgrade request are processed right away.
- **Closing**: `closeTimeout` is enforced, the server closes the TCP connection after answering a Close frame (RFC 6455 §7.1.1), a per-route `timeout()` no longer closes the connection, and the factory sees the path parameters.

#### `MSG_ZEROCOPY`

- **Buffers are kept until the kernel no longer reads them**: a close right after a response released buffers the kernel still sent from, so that a client could receive bytes of **another client's response**. Out-of-order completions are counted, and buffers are no longer reallocated or released while sends read them. See [MSG_ZEROCOPY](operations/performance.md#msg_zerocopy).
- **Slow clients receive whole responses**: a connection with sends in flight at `keepAliveTimeout` is closed once they complete, reset only without progress for a whole `keepAliveTimeout`.
- **No more `MSG_ZEROCOPY` on kTLS connections** (each large write failed first).

#### Compression and static files

- **Decoding**: truncated zstd bodies are rejected and multi-frame ones accepted (RFC 8878 §3.1), large gzip and deflate bodies close to `maxDecompressedBytes` are accepted in streaming mode, and data of 4 GiB and more is given to zlib in slices (it was truncated to 32 bits, partially compressed or corrupted).
- **Encoding**: the preference of the codings missing from a partial `preferredFormats` is no longer read uninitialized, gzip and deflate compression no longer aborts above `initialCompressionBufferLimit`, and a moved server keeps compressing with gzip and deflate (zlib binds streams to their address).
- **Static files**: links to directories are served as directories and dangling links answered `404` (were `500` and `503`), and a file sent over user-space TLS stops at the first failed write.
- **HTTP client**: an empty HTTP/1.1 body with a `Content-Encoding` is returned empty.

#### HTTP client

- **HTTP/2: response bodies must match their `content-length`** (`malformedResponse`), and **a response followed by `RST_STREAM(NO_ERROR)` is kept** (RFC 9113 §8.1). See [Error model](modules/http-client.md#error-model).
- **A rejected redirect `Location` no longer corrupts the request**, an invalid absolute path returning `invalidUrl` instead of throwing.
- **Idle connections to origins no longer requested are swept**, at most once per `keepAliveTimeout`.

#### Server lifecycle, configuration updates and data races

- **A failed config update leaves the server running with its previous configuration**: each update is applied atomically, the error logged, and a failed TLS reload is retried at the next update (an invalid certificate or unopenable access log stopped the server).
- **Lowering `maxHeaderBytes` or `maxBodyBytes` at runtime no longer stops a server with routes**: a per-route limit above the server one is capped to it. See [Per-route options](guides/routing.md#per-route-options).
- **`headerReadTimeout` only applies to HTTP/1.1 request heads**, for every request of a keep-alive connection (it closed HTTP/2 connections and tunnels with a `408`), and changing timeouts at runtime applies cleanly. See [Limits and timeouts](guides/requests.md#limits-and-timeouts) and [Updating a running server](guides/server-lifecycle.md#updating-a-running-server).
- **Lifecycle**: `MultiHttpServer::run()` no longer hangs on an early `beginDrain()` or when no worker starts, `beginDrain()` no longer turns a concurrent `stop()` back into a drain, `stop()` no longer tears down an event loop run by another thread nor terminates the process when called from a handler, a connection arriving while the server stops no longer crashes it, and the ephemeral port of a stopped server stays reserved (Linux, `reusePort`).
- **A client leaving in the middle of a file transfer no longer stalls later connections** reusing its state (macOS).
- **No more `SIGPIPE` on connections reset by the peer (Linux)**: plain sockets use `MSG_NOSIGNAL`, and the event loop (and `HttpClient` during TLS and file exchanges) keeps `SIGPIPE` blocked in its thread, the process-wide disposition untouched.
- **Data races found with ThreadSanitizer** (the whole suite runs clean with the new `AERONET_ENABLE_TSAN`): `stats()` from any thread, callback setters, `postConfigUpdate()`, router updates and `MultiHttpServer` start / stop races, logging from the signal handler, `beginDrain()`, and OpenSSL ex data indexes during static initialization.

#### Telemetry and metrics

- **OpenTelemetry span export no longer blocks the event loop**: spans are exported in batches by a background thread (each span was a synchronous OTLP POST, up to the 10 s exporter timeout).
- **`aeronet.bytes.read` counts every socket read**, and **`RequestMetrics::reusedConnection` is `false` for the first request of a connection**. See [Observability](operations/observability.md).

#### Platforms and build

- **Windows**: files can be sent several times (`TransmitFile` shared the file position), and read and write lengths above the Winsock limits are clamped.
- **macOS: an interest dropped from a watched socket is no longer reported** (`HttpClient` busy-polled).
- **Shared library builds link with every feature** (brotli was used without being linked), with `-z defs` on Linux.
- **Glaze builds with CMake 3.28 to 3.30 again**. See [Glaze with CMake older than 3.31](getting-started/install.md#glaze-with-cmake-older-than-331).

### 1.7.0 Improvements

#### New features and configuration

- **HTTP client streams response bodies** to a `ResponseBodySink` (`request(request, sink)`, `get(url, sink)`), de-chunked and decoded, one read in memory at a time. See [Streaming response bodies](modules/http-client.md#streaming-response-bodies).
- **`HttpClientConfig` loads from JSON and YAML files** (`LoadClientConfig()`, `aeronet-config-dump --client`), the configuration objects being reflected by Glaze member by member. See [Client configuration files](modules/json-and-config-files.md#client-configuration-files).
- **New `HttpClientConfig::inactivityTimeout`** (60 s): a stalled long download fails fast. See [Timeouts](modules/http-client.md#timeouts).
- **The HTTP client keeps its HTTP/2 connection when only a stream fails**, resetting that stream only. See [HTTP/2](modules/http-client.md#http2).
- **New `HttpClient::releaseUnusedMemory()`**, and **`HttpRequestView::tlsPeerSubject()`** to authorize mTLS clients. See [Mutual TLS](protocols/tls.md#mutual-tls).
- **TLS servers select ChaCha20-Poly1305 for clients preferring it** (`SSL_OP_PRIORITIZE_CHACHA`).
- **Messages**: `headerRemoveAllLines(name)` in a single pass, `deferWork()` accepts `void`, non default constructible and move-only results, `bodyInlineAppend()` appends to moved and static bodies, and constructors, `makeResponse()` and `makeRequest()` move owned bodies (`std::string&&`, vectors, `std::unique_ptr` buffers) instead of copying. See [Bodies](guides/responses.md#bodies) and [Building a response](guides/responses.md#building-a-response).
- **New zerocopy `ServerStats` counters**, and `Http2Config::maxRecycledRequests` (8).
- **A documentation site replaces `docs/FEATURES.md`**, with guides, protocol, module, reference and operations pages. See the [documentation site](index.md).

#### Performance

Benchmarks are in `benchmarks/internal/` (Release builds, A/B against 1.6.0 unless stated).

- **HTTP/1.1 server I/O**: adaptive read size from `minReadChunkBytes` (4 KiB) up to 256 KiB instead of the whole budget per connection, one read system call per request instead of two, `recv()` / `send()` / `sendmsg()`, idle keep-alive connections without input buffer, no wakeup system calls for deferred input, cheaper keep-alive bookkeeping and `Accept-Encoding` negotiation, pipelined requests answered by a single write (2.7 to 9 times faster batches, no 40 ms delayed ACK stalls), `TCP_CORK` only for flushes of several writes, and output waiting for a slow reader no longer copied (10-13% less CPU when streaming to a slow reader). See [Performance](operations/performance.md#how-a-request-is-served).
- **Smaller messages**: `sizeof(HttpResponse)` goes from 112 to 48 bytes, `sizeof(HttpRequest)` from 120 to 56 (a response move is 25-39% faster), and small `RawChars` / `RawBytes` grow by `malloc` + copy instead of `realloc`.
- **HTTP/2**: uploads allocated at once from their `content-length` (19-22% more requests/s on h2c), moved and static bodies sent without copy, no output buffer allocation per response (1 KiB batches 28-36% faster), faster request heads (a GET 14 to 33% faster), fewer indexed fields in responses (3.5 times faster encoding of three of them, as nghttp2), and faster frame parsing.
- **TLS and zerocopy**: read ahead and full records for gather writes (25% more requests/s for 176 KiB HTTP/2 uploads over TLS, 28 system calls instead of 74), and cheaper, adaptive `MSG_ZEROCOPY`. See [TLS record I/O](protocols/tls.md#tls-record-io) and [MSG_ZEROCOPY](operations/performance.md#msg_zerocopy).
- **WebSocket**: SIMD UTF-8 validation (25 GB/s with AVX2, 8 to 23 times faster for non-ASCII text, NEON too), unmasking in the same pass, large and compressed messages written without copy, zlib streams shared per event loop without context takeover (64 KiB echoes 77k to 116k messages/s, peak memory 103 MB to 20 MB with 384 connections). See [Callbacks](protocols/websocket.md#callbacks).
- **Telemetry batched**: counters summed in memory and exported at each maintenance tick, DogStatsD lines packed up to 8 KiB per datagram (the DogStatsD metrics of an HTTP/2 request take 0.58 µs instead of 7.6 µs). See [Batching and flush](operations/observability.md#batching-and-flush).

#### Build and tooling

- **New `AERONET_ENABLE_TSAN` option** (ThreadSanitizer builds).
- **Lighter development builds**: `AERONET_COMPACT_DEBUG_INFO` (split and compressed debug info, Debug test executables ~3.5 times smaller), and shared library builds no longer relink the executables when only a library implementation changed.
- **New `AERONET_UNITY_BUILD` option**: compilation CPU time of the libraries divided by 4 to 6 in Release. See the [unity build recipe](reference/configuration.md#unity-build).
- **Fetched dependencies only build what aeronet links** (`EXCLUDE_FROM_ALL`).

### 1.7.0 Others

- **Bumped opentelemetry-cpp from 1.29.0 to 1.30.0**.
- **`SingleHttpServer` split into components** (connection I/O, maintenance, HTTP/1.1 pipeline, TLS acceptor) without virtual calls nor measurable cost, and the callback types are available from `<aeronet/server-callbacks.hpp>`. See [Server architecture](development/server-architecture.md).
- **Internal callbacks are function pointers instead of `std::function`**: `HttpRequestView` is 24 bytes smaller, and `router()` no longer allocates. `RouterUpdateProxy::Dispatcher` is no longer public.

## [1.6.0] - 2026-10-03

### 1.6.0 Breaking changes

- **Header and trailer names are stored in lower-case** (`HttpRequest`, `HttpResponse`, `HttpResponseWriter`, client responses, views, iterators, HTTP/1.x serialization). Every API taking one name (`hasHeader`, `headerValue*`, `header`, `headerAddLine`, `headerAppendValue`, `headerRemove*`, `hasTrailer`, `trailerValue*`, `trailerAddLine`) takes a `LowerAsciiKey`: uppercase literals fail to compile and dynamic keys must be normalized first. Title-case field names are no longer preserved on HTTP/1.x (lower-case is valid in HTTP/1.x and required by HTTP/2). Empty header names are rejected.
- **Safer defaults, `0` no longer means "unlimited"**:
  - `StaticFileConfig::maxMultipartRanges`: `0` now rejects every range query with `416`;
  - `Http2Config::maxStreamsPerConnection`: default `1000000`;
  - `DecompressionConfig::maxCompressedBytes`: default `128 MiB`, and `maxExpansionRatio`: default `1000.0`;
  - `DecompressionConfig::streamingDecompressionThresholdBytes = 0` now streams every body (it used to mean aggregated mode);
  - `HttpServerConfig::zerocopyMinBytes` cannot be `0` (`10 KB` or more recommended).
- **TLS 1.2 or newer by default**: TLS 1.0 and 1.1 handshakes are refused unless `TLSConfig::minVersion` is set explicitly.
- **HTTP methods are case-sensitive** (RFC 9110 §9.1): `get` or `GeT` is an unknown method answered `501` (and a denied CORS preflight). Accepting them allowed bypassing intermediary rules written for the exact method name.
- **API**:
  - `StaticFileConfig` and `DecompressionConfig` field order changed (`decoderChunkSize` is a `uint32_t` and the second field of `DecompressionConfig`): check aggregate initializations;
  - `CorsPolicy::PreflightResult::response` is a `std::optional<HttpResponse>`, only set when the preflight is allowed;
  - `InMemoryTokenBucketRateLimitStore` has no default constructor: `explicit InMemoryTokenBucketRateLimitStore(uint8_t nbShards)`;
  - `RetryConfig::retryStatuses` is a set-like container;
  - `MiddlewareResult::Decision` was removed;
  - `HttpClientConfig::validate()` rejects nonsensical timeouts and retry settings (`std::invalid_argument`);
  - `Http2Config::enablePush` and `enableH2cUpgrade` are deprecated and have no effect (kept so that existing code and configuration files compile and load).

### 1.6.0 Bug Fixes

- **Zerocopy: closing a connection no longer releases buffers the kernel still sends**: with `MSG_ZEROCOPY` (`Opportunistic` by default, for responses of at least `zerocopyMinBytes` to non-loopback peers), a connection closed right after its response released buffers the kernel still sent or retransmitted from, so once reused a client could receive bytes of another client's response. A graceful close now waits for the sends to complete, and a forced close with sends in flight resets the connection so that the kernel drops them.
- **A client that stops reading no longer blocks the server**: unsent HTTP/1.1 output now waits for the write-readiness event instead of being retried, so a single client not reading a large or streamed response no longer stalls every connection of its server thread. Later responses and chunks queue behind it (a streaming `writeBody()` returns false once `maxOutboundBufferBytes` is exceeded).
- **Strict HTTP/1 parsing** (several found with the [HTTP Garden](https://github.com/narfindustries/http-garden) payloads):
  - a bare CR in a request head is rejected immediately with `400`;
  - chunked bodies: an empty chunk size and control characters (bare LF) in chunk extensions get `400`, a chunk size over 64 bits no longer wraps around to `0` (which ended the body early and let the following bytes be parsed as another request) and gets `413`, and a `Content-Length` close to `SIZE_MAX` no longer overflows;
  - request-targets are validated: control characters are rejected (also in HTTP/2 `:path`), only origin-form, absolute-form, authority-form (`CONNECT`) and asterisk-form (`OPTIONS`) are accepted, and a decoded `%00` in the path is rejected (all `400`);
  - `Connection: close` is honored anywhere in the `Connection` list (RFC 9110 §7.6.1), and an HTTP/1.0 connection is only kept alive with an explicit `keep-alive`;
  - the client validates header names and values of HTTP/1.1 responses, and `HttpClientConfig::maxResponseBytes` counts header bytes as well as the body;
  - HTTP/2 field values with leading or trailing whitespace are rejected as malformed (RFC 9113 §8.2.1).
- **Injection**: replacing a header with `header()` (also used by `location()`) did not validate the value and `reason()` accepted any character, so CR/LF could split the response (both now throw `std::invalid_argument`). `%00` / `%1F` in query strings, the internal separators of decoded parameters, stay encoded (`?a=1%00admin%1Ftrue` could inject a second parameter). `headerAppendValue()` validates the value and separator, and header or trailer values with surrounding whitespace are trimmed or rejected consistently (`trailerAddLine`, concatenated headers in `HttpResponse` constructors).
- **Trailing slash redirects no longer stop the server**: with `TrailingSlashPolicy::Redirect`, a path such as `/users/a%0d%0a/` matching a parameterized route copied CR/LF into `Location` and the resulting exception escaped the event loop. The path is now percent-encoded.
- **HTTP/2**:
  - request bodies no longer stall when a peer (h2load) advertises a large initial window: new streams used the peer's setting for both directions, so the server sent no `WINDOW_UPDATE`;
  - `Http2Config::enablePush = true` made the server advertise `SETTINGS_ENABLE_PUSH = 1`, which RFC 9113 §6.5.2 forbids and curl answers by closing the connection: servers and clients now always advertise `0`, and the client treats a server advertising `1` as `PROTOCOL_ERROR`;
  - cleartext `Upgrade: h2c` requests are answered over HTTP/1.1 instead of hanging (deprecated mechanism, RFC 9113 §3.1; prior-knowledge cleartext HTTP/2 remains available through `Http2Config::enableH2c`);
  - async handlers now resume on prior-knowledge h2c connections (the connection was not flagged as HTTP/2);
  - the HTTP/2 TLS client reuses pooled connections with pending control frames (`WINDOW_UPDATE`) instead of reconnecting.
- **Server**:
  - HTTP/1.1 `CONNECT` no longer stalls on the first tunnel bytes buffered with the head;
  - static file ranges follow RFC 9110 (case-insensitive units, ignored on `HEAD`, multipart parts in request order, malformed and unsatisfiable ranges distinguished);
  - `beginDrain()` is no longer lost when called while a server is still starting up (it waits for the startup, and the deadline uses a fresh clock);
  - an exception in a streaming handler returns `500`, and streaming handlers are no longer called for HTTP/1.0 requests;
  - CORS request header names (`Access-Control-Request-Headers`) are matched case-insensitively, as browsers send them lower-cased (names given to `allowRequestHeader()` / `exposeHeader()` are deduplicated case-insensitively);
  - a throwing `startDetachedAndStopWhen()` / `startDetachedWithStopToken()` predicate no longer crashes on Windows, and a data race in `AsyncHandle::stop()` / lifecycle state is fixed (ThreadSanitizer).
- **`HttpMessage` consistency**:
  - body headers stay consistent when an empty `file()` body is replaced, when a body is replaced by a directly compressed one (`Content-Encoding` was lost), and when headers are added during direct compression (they were overwritten by the final `Content-Length`);
  - `Content-Type`, `Content-Length` and `Content-Encoding` can no longer be modified through the generic `headerAppendValue` / `headerRemoveValue` (same rules as `header`, `headerAddLine`, `headerRemoveLine`);
  - `headerRemoveValue` trims the value and no longer corrupts headers when removing an empty token (`"a, b"` became `"ab"`);
  - `Content-Type` values as short as `a/b` are accepted by all body setters and in client responses, and replacing a `Content-Type` containing `:` wrote at a wrong position;
  - streaming `HttpResponseWriter::file()` sends a single `Content-Type`, and `HttpResponseWriter` throws on an invalid `Content-Type`.
- **Memory safety**: possible out-of-bounds write in direct compression finalization, out-of-bounds read when removing a captured or file body with a user `Content-Encoding`, and a moved `HttpClient` (or `SingleHttpServer` with DogStatsD) reading the freed configuration of the moved-from object.
- **Client**: a request deadline wins over socket readiness arriving after it (`HttpClientErrc::timeout`).
- **Builds**: fixed compilation with `AERONET_ENABLE_HTTP2=OFF` and `AERONET_ENABLE_WEBSOCKET=ON`, with `AERONET_ENABLE_HTTP2=ON` and `AERONET_ENABLE_HTTP_CLIENT=OFF`, and in Release without `AERONET_ENABLE_TEST_HOOKS`.

### 1.6.0 Improvements

- **Bounded deferred output and zerocopy retention**: `maxOutboundBufferBytes` now covers HTTP/2 queued frames and flow-control-deferred in-memory responses. HTTP/2 pauses input at the output high-water mark, refuses new streams when the connection budget is exhausted, answers `503` for fixed responses that cannot fit, and resets streaming responses exceeding the new `Http2Config::maxStreamPendingBytes`. The new `maxZerocopyPendingBytes` ceiling stops using zerocopy on a connection before another retained payload would cross it (sends already in flight stay tracked until completion).
- **Advanced TLS operations and hardening**: passive stapling of cached DER OCSP responses (globally or per SNI certificate), client certificate validation with PEM / DER CRLs or an application revocation hook, NSS key log in debug builds only, server cipher preference, renegotiation disabled where supported, SNI wildcards matching a single label, and scrubbing of private keys and session ticket keys on replacement or destruction.
- **Labeled HTTP/2 telemetry**: metric calls accept non-owning per-measurement label spans (OpenTelemetry attributes, DogStatsD tags). HTTP/2 reports frame counts and bytes by direction and type, stream lifecycle, request duration and body size, HPACK block sizes and compression ratios.
- **HTTP/2 performance**: adaptive outbound batching (small responses share the output buffer so TLS encrypts a batch in one write, larger payloads are gather-written without recopy), receive-window replenishment batched at a half-window low-water mark, a lazily built hash index for enlarged HPACK dynamic tables (lookups from 109 / 395 ns to about 9 ns at 16 / 64 KiB), and `Http2Connection` shrunk from 728 to 524 bytes.
- **Server performance**: `Date` header cached per reactor (about 0.6 ns instead of 35 ns), transport dispatch without vtables or allocation for plain connections, header lookups without case folding on every comparison, CRLF search through libc `memchr`, lock-sharded rate limiter, fewer copies in the WebSocket upgrade and for a large first `HttpResponseWriter` chunk, no more `Http2Stream` push states.
- **Client performance**: zero-copy hand-off of HTTP/1.1 response bodies, HTTP/2 file uploads without a scratch copy (a file shorter than declared fails with `HttpClientErrc::writeError`), single-pass cache pruning, and a poll timeout adapted to request timeouts under 25 ms.
- **New settings and behaviors**: `Http2Config::maxPriorityTreeDepth` is enforced (deeper or cyclic dependencies clamp to the root), `AccessLogConfig::flushThresholdInBytes` (8192), `HttpClientConfig::minReadChunkBytes` (16 KB), `HttpClientConfig::cache.maxRequestSize`, `CompressionConfig::useLeadingZeroesInContentLength` (padding is now leading zeroes instead of trailing spaces), `Http2Config::sendContentLengthHeader` (default `true`), `HttpClient` is copyable, built-in error responses include the global headers, and HTTP/1.x requests with a higher minor version (e.g. `HTTP/1.3`) are served as HTTP/1.1 instead of `505`.

### 1.6.0 Others

- AddressSanitizer is no longer on by default in Debug builds: set `AERONET_ENABLE_ASAN=1`.
- The server can be left out of the build with `-D AERONET_ENABLE_HTTP_SERVER=OFF` (client only).
- Each configuration object's Glaze adapter moved into its own header; `zstd.h` and `brotli/encode.h` removed from internal headers; `aeronet/version.hpp` added to `aeronet-client.hpp`.
- Unknown HTTP/2 `SETTINGS` ids are logged at debug level.
- glaze 8.1.0 to 8.4.0, opentelemetry-cpp 1.28.0 to 1.29.0.

## [1.5.0] - 2026-08-20

### 1.5.0 Breaking changes

- **HTTP CONNECT tunneling is disabled by default**: an empty `HttpServerConfig::connectAllowlist` rejects every target with `403`, closing an unauthenticated SSRF / open proxy in default configurations (HTTP/1.1 and HTTP/2). Configure trusted hosts with `withConnectAllowlist()` (or the `connectAllowlist` setting); `["*"]` restores the previous unrestricted behavior, loopback, private-network and cloud metadata targets included.
- **Lower-case keys for header and trailer lookups on `HttpRequestView`**: `headerValue`, `headerValueOrEmpty`, `trailerValue`, `trailerValueOrEmpty`, `hasHeader` and `hasTrailer` take a `LowerAsciiKey` (`headerValue("Host")` no longer compiles, `headerValue("host")` does). `headers()` and `trailers()` return a case-sensitive map with lower-case keys (`find("X-Header")` can no longer match).
- **`http::Connection`, `http::ContentType`, `http::Host`... are `LowerAsciiKey`** instead of `std::string_view` (validated at compile time). Implicit conversion back keeps typical code unchanged; `*Sep` formatting constants stay `std::string_view`.
- **`File::Identity` is private and `File::identity()` is replaced by `File::appendIdentityData(char*)`** (buffer of at least `File::kIdentitySize` bytes).
- Global header values must be trimmed of OWS.

### 1.5.0 Bug Fixes

- **Async handlers: heap-use-after-free when a connection closed during a long `deferWork()`**: the keep-alive sweep could free the connection while background work ran, which then wrote its result into freed memory. The worker no longer touches the request or coroutine frame (state co-owned through a `shared_ptr`), connections with an active async handler are excluded from the keep-alive sweep, and a per-connection generation token prevents a stale completion from resuming another connection that reused the same fd (HTTP/1.x and HTTP/2).
- **A handler slower than `keepAliveTimeout` no longer gets its own response swept as idle**: the idle window now restarts when the work completes, and active HTTP/2 streams (including those waiting for flow-control credit) are excluded from keep-alive reaping. Clients used to get a `200` with fewer bytes than `content-length`.
- **Stricter request validation**:
  - HTTP/1.1 requests without `Host` get `400` (RFC 9112 §3.2);
  - HTTP/1.1 field names must be valid tokens and values must not contain NUL, bare CR or bare LF;
  - HTTP/2 field names with uppercase or invalid bytes and values with NUL/CR/LF are rejected (RFC 9113 §8.2.1);
  - malformed HTTP/2 pseudo-header sections (duplicate, misordered, undefined, response-only, missing, invalid CONNECT shapes) reset the stream with `PROTOCOL_ERROR` without closing the connection (field sections are still decoded in full to keep HPACK in sync); unsupported extension methods return `501` instead of being dispatched as `GET`;
  - `Http2Config::maxHeaderListSize` is enforced on decoded headers and trailers (`ENHANCE_YOUR_CALM`).
- **HPACK dynamic table desynchronization**: entries are added on incremental indexing even when the header is rejected as malformed (RFC 9113 §4.3).
- **Startup, shutdown and lifecycle**: route updates submitted while a server was starting could race the router (a synchronized `Starting` state now queues them), and predicate or stop-token shutdown between event-loop iterations could leave the listener open without an event loop (intermittent 10 s Windows CI stalls).
- **Client**: heap-buffer-overflow when adding a header to a body-less request built with reserved capacity (`makeRequest(additionalCapacity, method, url)`).
- **macOS**: `EventLoop::add()` no longer masks a failed filter registration (each filter's result is checked with `EV_RECEIPT`).

### 1.5.0 Improvements

- **Router**: literal-route slots store compact side-table indices and `RoutingResult` references immutable route metadata (96 to 48 bytes, `Router::match` 39% smaller, about 45% lower median latency for literal hits).
- **HTTP/1.1 client responses**: completed chunked and decompressed bodies transfer suitably sized scratch allocations into `HttpResponse` (256 KiB chunked parse about 39% faster), and parsing is one stricter forward pass with SIMD CRLF search (25% faster for 1 KiB chunks, 68% for 16-byte chunks). The client rejects bare-LF framing, like the server.
- **HPACK**: smaller Huffman lookup table with correct padding handling, collision-free perfect hash for the static table and cheaper dynamic table insertion and eviction (up to 46% faster decoding, 30% faster round trips, 63% faster static lookups).
- **Request parsing**: faster header line parsing (about 40% for typical browser headers, 30% for API proxy headers), faster case-insensitive hashing and method parsing (10% for few headers, up to 23% for many), faster MIME lookup (about 40%), custom `WriteInt` instead of `std::to_chars`, and lookups normalized to lower case.
- The 16-setting limit of `SETTINGS` frames was removed (the spec has none).
- New MkDocs Material documentation site (GitHub Pages, with the live benchmark dashboards) and a benchmark profiling workflow (`scripts/profile_benchmark.sh`, flamegraphs).
- Harmless MSVC and GCC warnings fixed in `ndigits.hpp`; internal `<aeronet/stringconv.hpp>` removed. glaze bumped to 8.1.0.

## [1.4.1] - 2026-07-25

- **Fix compilation with OpenSSL**: CMake did not export the `aeronet_tls` static library publicly.
- **HTTP client: HTTPS verification on minimal Linux images**: with no explicit CA and peer verification on, the client relied only on OpenSSL's compiled-in default directory, often absent from minimal containers, so every handshake failed. It now also probes well-known CA locations (e.g. `/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt`) when `SSL_CERT_FILE` / `SSL_CERT_DIR` are not set.
- New `HttpClient::makeRequest(std::size_t additionalCapacity, std::string_view method, std::string_view url)` to add headers before the body without reallocation.

## [1.4.0] - 2026-07-23

### 1.4.0 Breaking changes

- **`HttpRequest` renamed `HttpRequestView`** (non-owning). `HttpRequest` is now an owning type used by the `HttpClient` API to build a request, mirroring `HttpResponse`.
- HTTP/2-specific `HttpRequestView` APIs (`isHttp2()`, `streamId()`, `scheme()`, `authority()`) are only compiled with `AERONET_ENABLE_HTTP2`.
- JSON/YAML helpers (`HttpResponse::bodyJson` / `bodyYaml`, `HttpRequestView::bodyAs` / `bodyAsYaml`) need `<aeronet/http-json.hpp>` (the `<aeronet/aeronet.hpp>` umbrella is unaffected), and `aeronet::log` needs `<aeronet/log.hpp>`: Glaze and spdlog no longer leak into core headers.

### 1.4.0 New features

- **HTTP client (`HttpClient`, `AERONET_ENABLE_HTTP_CLIENT`)**: a synchronous client built on aeronet's own transport and event loop, speaking HTTP/1.1 and HTTP/2 (HPACK and frame codecs shared with the server; `HttpClientConfig::httpVersion` `Auto` / `Http2` / `Http1_1`, ALPN over TLS, prior-knowledge h2c). It provides HTTPS (peer and hostname verification, TLS bounds, custom CA, ciphers, mTLS), per-origin keep-alive pooling, redirect following, a retry policy with backoff, transparent compression and decompression, request trailers, request bodies streamed from a file (`HttpRequest::file()`), cleartext forward proxy (`withProxy`, `CONNECT` tunnel for HTTPS origins) and an opt-in response cache (`withCache`). Requests return an `HttpClientResult` (`std::expected<HttpResponse, HttpClientErrc>`). A client is not thread-safe (one per thread).
- **JWT module (`aeronet_jwt`, `AERONET_ENABLE_JWT`)**: sign and verify JWS tokens (RFC 7519; HMAC, RSA, PS, ECDSA, EdDSA) over OpenSSL, with claim validation (`exp`, `nbf`, `iss`, `aud`, `sub`, leeway, injectable clock), JWK/JWKS parsing with `kid` selection, `alg:none` always rejected, algorithm-family anti-confusion and constant-time HMAC. Exception-free, JWE out of scope, plus a `base64url` codec. See the [JWT guide](modules/jwt.md).
- **HTTP to HTTPS redirect**: `HttpServerConfig::withHttpsRedirect(targetHttpsPort = 443, statusCode = 301)` answers every plaintext request with a redirect built from the `Host` header (path and query re-encoded, `400` without `Host`). Not combinable with `tls.enabled` on the same listener.
- **Rate-limiting middleware**: `RateLimitRequestMiddlewareBuilder` with configurable client keys (peer address, `X-Forwarded-For`, custom header or extractor), an in-memory token bucket (`429` with `Retry-After`), global or per-route / per-group installation, and an optional Redis sliding-window contract.
- **Dedicated probe listener for `MultiHttpServer`**: `BuiltinProbesConfig::withDedicatedPort(port)` serves `/livez`, `/readyz` and `/startupz` from a separate event loop, with heartbeat-based liveness (unhealthy only when every worker's heartbeat is stale beyond `livenessStaleThreshold`, 10 s by default). See the [Kubernetes guide](operations/kubernetes.md#isolating-probes-on-a-dedicated-port).
- `HttpRequestView::clientAddress()` (HTTP/1.1 and HTTP/2) and **HTTP/2 request trailers** (`trailers()` / `trailerValueOrEmpty()`; pseudo-header fields in trailers, or a trailing block not ending the stream, reset the stream).

### 1.4.0 Bug fixes

- Request trailers could leak across keep-alive requests (the trailer length was not reset for non-chunked bodies).
- HTTP/2: responses larger than the peer's flow-control window never completed (the remainder is now deferred and flushed as `WINDOW_UPDATE`s arrive, trailers included); active-stream accounting leaked for responses completed outside frame processing (eventually `Max concurrent streams exceeded`); field values with trailing whitespace, rejected by nghttp2 / curl (RFC 9113 §8.2.1), are now trimmed before HPACK encoding.
- TLS: large responses could be truncated by one 16 KiB record when a fatal error left on the shared OpenSSL error queue was misclassified against the next connection; `requireClientCert` was silently ignored when `requestClientCert=false` (the invariant is now enforced on every configuration path).
- HTTP/1.1: empty-body responses omitted `Content-Length` (defeating keep-alive; `Content-Length: 0` is now synthesized where allowed), and the status line omitted the mandatory SP before an empty reason phrase (`HTTP/1.1 200 \r\n`).
- `bodyAs<T>()` / `bodyAsYaml<T>()` could over-read a non-null-terminated body, a stale poll event could crash on macOS (out-of-range fd), a Debug-only compile error in `fullVersionStringView()` was fixed, and built-in server error bodies carry a `Content-Type`.

### 1.4.0 Improvements

- Lower compile-time footprint of core headers (Glaze and spdlog moved out), `aeronet` split into `aeronet_server`, `aeronet_client` and an `aeronet` umbrella target.
- Static files: per-file `ETag` / `Last-Modified` / `Content-Type` fragments are precomputed and validated on size and mtime (`StaticFileConfig::headerCacheCapacity`, 1024 by default, O(1) LRU).
- Faster `CaseInsensitiveEqual` (16 bytes at a time with SSE2 / NEON, 35-57% on strings of 16 bytes or more) and small `Copy` / `Append` (about 1.5x on a realistic mix); better HPACK reserve.
- `HttpServerConfig::nbThreads` narrowed to `std::uint16_t`; status line limit raised from 64 KiB to 16 MiB and `reason` limit from 1024 to 65522; scripted HTTP/2 client benchmarks (vs libcurl, `h2c` and `h2`-TLS) added to CI.

## [1.3.0] - 2026-06-01

### 1.3.0 New features

- **Route parameter constraints**: `{name:pattern}` (e.g. `/users/{id:[0-9]+}`), compiled at registration, with a fast matcher for character classes and a `std::regex` fallback.
- **Route groups** (`RouteGroup` via `Router::group(prefix)`): non-owning prefix proxies sharing configuration, CORS policy and middleware, nestable with inheritance.
- **Per-route configuration**: `requestTimeout` (`408`, per-stream on HTTP/2), `maxBodyBytes` (`413`) and `maxHeaderBytes` (`431`), through `PathEntryConfig` or the chainable `.timeout()`, `.maxBodyBytes()`, `.maxHeaderBytes()`.
- **Configuration from YAML / JSON files** with `AERONET_ENABLE_GLAZE` (`SingleHttpServer` and `MultiHttpServer` constructors taking a path).
- **Structured access logging** (`HttpServerConfig::accessLog`): `clf` or `json`, sinks `none`, `stdout`, `file`, optional `X-Forwarded-For`, independent from spdlog.
- **Event loop**: adaptive poll timeout (`pollIntervalMinFactor` / `pollIntervalMaxFactor`, `withPollIntervalFactors()`), intrusive min-heap keep-alive deadline queue (no scan of every connection per tick), and `HttpServerConfig::maxAcceptBatchSize` (64) against connection-burst starvation.
- **Deterministic network fault injection** test infrastructure (`FaultPolicy`, `TestPipe`, `FaultInjectingTransport`, compile-time gated by `AERONET_ENABLE_TEST_HOOKS`).

### 1.3.0 Bug fixes

- `flat_hash_map::erase(iterator)` returned the wrong iterator when a colliding entry was relocated (erase-while-iterating could skip entries).
- HTTP/1.1 parser header boundary off-by-one when the buffer ended exactly at a header CRLF (assertion or corrupted state).
- WebSocket `processInput()` over-erased the input buffer when carry-over input existed.
- Connection stalls: a `maxPerEventReadBytes` cut leaving data in the TCP buffer never raised another edge-triggered event (partially read fds are now re-processed next iteration), and a tunnel hung when `epoll_ctl(EPOLL_CTL_MOD)` failed.
- `minReadChunkBytes` is honored as the actual read size; HTTP/2 admits peer-initiated streams against the local `SETTINGS_MAX_CONCURRENT_STREAMS`; no empty `HEADERS` frame for deferred file sends without trailers.

### 1.3.0 Breaking changes

- `HttpServerConfig::maxHeaderBytes` (and `withMaxHeaderBytes()`) is a `std::uint32_t` (was `std::size_t`).
- `HttpServerConfig::maxPerEventReadBytes` must be `> 0` (`std::numeric_limits<uint32_t>::max()` approximates unlimited) and defaults to `128 KiB` instead of unlimited, for fairness.

### 1.3.0 Improvements

- HPACK: hash lookup in the static table (about 33.5% on `BM_HpackFindHeader`) and reuse of evicted-entry buffers in the dynamic table (about 46.9% on encode, 19.9% on round trips).
- Fewer allocations and copies: WebSocket frame building, `HttpResponseWriter` compression buffer reuse, HTTP/2 per-stream map consolidation, async handler state allocated only for connections using them.
- Backpressure rejects extra response chunks immediately once `maxOutboundBufferBytes` marks a connection for drain.
- WebSocket benchmarks (k6: echo, mix, ping-pong, churn, compression, against uWebSockets and Drogon) added to CI.

## [1.2.0] - 2026-03-23

`aeronet` now also runs on **macOS** (kqueue backend) and **Windows** (WSAPoll backend).

### 1.2.0 Breaking changes

- `HttpServerConfig::tcpNoDelay` is an enum with an `Auto` mode (default: `TCP_NODELAY` for HTTP/2 over TLS only). `withTcpNoDelay()` is unchanged.

### 1.2.0 New features

- **HTTP/2 `CONNECT` tunneling** (RFC 7540 §8.3): per-stream bidirectional tunnels, allowlist enforcement, cleanup on reset or close.
- **Truly asynchronous HTTP/2 handlers**: `co_await req.deferWork(...)` suspends per stream (HTTP/2 used to drain async handlers synchronously, blocking the whole connection).
- **Multi-range responses** (`multipart/byteranges`, RFC 7233): ranges sorted and coalesced, bounded by `maxMultipartRanges` and `maxMultipartBodySize`.
- `StreamingHandler` supports HTTP/2.

### 1.2.0 Bug fixes

- HTTP/2 `:path` query parsing now mirrors HTTP/1.1 (in-place percent-decoding); `Router` no longer asserts on a path sharing a prefix with a registered one but diverging at an unmatched segment; a malformed CRLF ending a chunked body returns `400` instead of waiting.
- OpenSSL error queue is cleared after `SSL_shutdown()` (stale errors misclassified on connection reuse); `ENOBUFS` on zerocopy responses falls back to copying; zerocopy data corruption under sustained load on virtual network devices fixed by draining completion notifications before each send (and `ENOBUFS` handling on TLS + kTLS).
- `CompressionConfig::maxCompressRatio` is strictly respected; a race in multi-threaded `stop()` and sweeping of tunnel connections were fixed.

### 1.2.0 Improvements

- `StaticFileHandler`: small files (128 KiB threshold by default) are read into memory instead of zero-copy transfer, plus faster directory listing, metadata and `sendfile` chunking.
- Compression: streaming compression starts with a small chunk size growing exponentially, brotli encoder state and zstd contexts are reused, and `HttpResponseWriter` no longer over-allocates for brotli and zstd.
- Connections are stored in a vector indexed by fd; no `memmove` for non-prepared HTTP/2 bodies; fewer `steady_clock::now()` calls; `std::memchr` CRLF search; binary search for HPACK static names; `HttpServerConfig::zerocopyMinBytes` (128 KiB); compression telemetry counters (`aeronet.http_responses.compression.*`); `WebSocketDeflate` validation; HTTP/2 hardening and metrics.
- **zlib-ng** (native mode, SIMD) replaces zlib for gzip / deflate and WebSocket `permessage-deflate` (`AERONET_ENABLE_ZLIBNG=OFF` keeps classic zlib, bumped to 1.3.2).
- New `fullVersionWithRuntime()`, clang-format 21, HTTP/2 benchmarks with `h2load` in CI.

## [1.1.0] - 2026-02-16

### 1.1.0 Breaking changes

- `HttpServerConfig::globalHeaders` must be key-value pairs separated by `http::HeaderSep`; invalid header names or values inserted in a response, or malformed concatenated headers in `HttpResponse` constructors, throw `std::invalid_argument`.
- Setting `Content-Type` and `Content-Length` through `header` / `headerAddLine` is prohibited (it was undefined behavior): use `contentType()` / `contentLength()` for streaming handlers, and set the content type with the body otherwise.
- `HttpRequestView::queryParams()` is now a map-like view collapsing duplicate keys (last value wins); `queryParamsRange()` keeps the duplicate-preserving iteration. `telemetryContext()` was removed from `HttpServer` / `SingleHttpServer` and telemetry metric methods (`DogStatsD` included) are no longer `const`.

### 1.1.0 New features

- **Direct compression**: inline bodies created through `HttpRequestView::makeResponse()` can be compressed at `body()` / `bodyAppend()` time (`DirectCompressionMode` `Auto` / `Off` / `On`, `CompressionConfig::defaultDirectCompressionMode`). See [Direct Compression](guides/compression.md#direct-compression).
- `HttpRequestView::makeResponse()` factories, `HttpRequestView::deferWork()` (run a task on a dedicated thread and come back to the event loop), `size` / `length` / `reserve` helpers on `HttpResponse`, and `HttpServerConfig::addTrailerHeader` (HTTP/1.1).
- New `HttpRequestView` helpers: `hasHeader`, `hasTrailer`, `hasPathParam`, `hasQueryParam`, `pathParamValue*`, `queryParamValue*`, `queryParamInt`, `headerRemoveLine`, `headerRemoveValue`. Router paths accept an asterisk as a non-terminal segment (`/files/*/metadata`).
- **`MSG_ZEROCOPY` support** (plain text and kTLS, Linux) through `HttpServerConfig::withZerocopyMode()` (`Disabled`, `Opportunistic` default, `Enabled`).
- New `AERONET_ENABLE_ASYNC_HANDLERS` option and an experimental C++20 module interface file.

### 1.1.0 Bug fixes

- `Content-Length` updated correctly by `bodyAppend()` on a captured body; **HEAD** responses with trailers omit the body and no longer switch to chunked encoding (RFC 7230 §4.3.2).
- `Accept-Encoding` parsing is closer to RFC 7231 §5.3.4 (highest `q` among duplicate encodings), and `h2c` connections handle the HTTP/1.1 `Upgrade` mechanism.

### 1.1.0 Improvements

- **Router radix tree** (about 40% faster pattern matching, lower memory), faster case-insensitive header hashing (FNV-1a), codec contexts reused, in-place compression of captured payloads, faster `HttpResponse::file()`, exponential `bodyAppend` growth, and a single shared compression buffer per server instead of per connection.
- Header values stored in `HttpResponse` / `HttpResponseWriter` are trimmed; `DogStatsD` reconnects automatically; `WebSocketConfig::maxMessageSize` is strictly enforced on decompression; prepared responses avoid body and trailer allocation for **HEAD**.
- `body(std::string&&, ...)` and `body(std::vector<std::byte>&&, ...)` only capture rvalues (an lvalue `std::string` selects the inline `std::string_view` overload, avoiding silent copies); an invalid chunked length returns `400` instead of `413`; `CompressionConfig::maxCompressRatio` default raised from `0.5` to `0.6`.
- ARM64 in CI, [Crow](https://github.com/CrowCpp/Crow) in the benchmarks, a `body-codec` scenario, and benchmark configurations split by connection count (default 50 per thread).

## [1.0.0] - 2026-01-17

First production-ready release of a modern, high-performance HTTP/WebSocket server library for Linux (C++23, GCC 13+ / Clang 21+, OpenSSL 3+ optional).

### Features

- **HTTP/1.1**: strict RFC 7230 parsing, keep-alive and pipelining, chunked bodies with trailers, size limits, streaming responses with backpressure, `HEAD`, `CONNECT`, `Expect: 100-continue`, configurable `TRACE`, duplicate header merging.
- **HTTP/2 (RFC 9113)**: HPACK, multiplexing with stream and connection flow control, ALPN `h2`, h2c prior knowledge and `Upgrade`, the same `HttpRequestView` API as HTTP/1.1, optional `PRIORITY`, CORS and middleware. (Server push is intentionally not implemented.)
- **Compression**: gzip, deflate, zstd and brotli (feature-gated), `Accept-Encoding` negotiation with q-values, inbound body decompression with safe limits (`413` / `415`), automatic `Vary`.
- **TLS**: file or in-memory PEM, SNI multi-certificate routing, optional or required mTLS, strict ALPN, TLS 1.2+, handshake timeout, session tickets with key rotation, kernel TLS with `sendfile`, hot reload of certificates and trust store, handshake and cipher metrics.
- **Static files (RFC 7232 / 7233)**: zero-copy `sendfile`, single-range requests, conditional requests, strong ETags, directory listing, path traversal protection.
- **WebSocket (RFC 6455)** with per-message compression, **streaming responses** (`HttpResponseWriter`), and **routing**: exact, parameterized (`/users/{id}`) and wildcard paths, global and per-path middleware, trailing slash policies.
- **Architecture and operations**: single-thread epoll event loop per server scaled with `SO_REUSEPORT` (`HttpServer` wrapper), Kubernetes probes (`/livez`, `/readyz`, `/startupz`), OpenTelemetry, DogStatsD, a JSON stats endpoint, per-server telemetry state and graceful drain.

### Security

- Overflow-checked buffers (`std::overflow_error`), `SafeCast` and compile-time checks against integer wraparound, configurable header and body limits (`431` / `413`), chunk size validation, `maxOutboundBufferBytes`, and multipart part, header and size limits.
- Static file path traversal protection (canonical paths, `..` segments rejected, `symlink_status()` checks, root confinement).
- Forbidden trailer headers rejected, `400` on malformed input, opt-in Slowloris header timeout, decompression expansion guards, `415` on unknown content codings, HTTP/2 frame and flow-control validation.
- ASAN / UBSAN and clang-tidy in CI, about 94% code coverage, no global mutable state.

### Known limitations (planned for later releases)

HTTP/2 `CONNECT` (use HTTP/1.1), multi-range responses, pluggable logging sinks, deterministic network fault tests and OCSP stapling.

### Compatibility

No breaking changes from 0.8.x. Build instructions are in [docs/getting-started/install.md](getting-started/install.md), the live benchmark dashboard at <https://sjanel.github.io/aeronet/benchmarks/>, security reports go to <dev.sjanel@gmail.com> (not public issues), and the roadmap in [docs/ROADMAP.md](ROADMAP.md).
