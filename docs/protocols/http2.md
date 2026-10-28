# HTTP/2

aeronet implements HTTP/2 ([RFC 9113](https://www.rfc-editor.org/rfc/rfc9113)) with HPACK header compression ([RFC 7541](https://www.rfc-editor.org/rfc/rfc7541)). Handlers are protocol-agnostic: the same router and handlers serve HTTP/1.1 and HTTP/2 requests, and HTTP/2 brings stream multiplexing, flow control, and header compression without changes to application code.

HTTP/2 is built when the `AERONET_ENABLE_HTTP2` CMake option is `ON` (the default). When it is `OFF`, the `aeronet/http2` module is not compiled and the HTTP/2 configuration API (`HttpServerConfig::withHttp2()`, `enableHttp2()`) does not exist. Once built, HTTP/2 is enabled by default at runtime (`Http2Config::enable`).

## How a connection becomes HTTP/2

| Transport | Mechanism | Configuration |
| --- | --- | --- |
| TLS | The client selects `h2` through ALPN during the handshake. | List `"h2"` in `withTlsAlpnProtocols()`, before `"http/1.1"` to prefer HTTP/2. Requires `AERONET_ENABLE_OPENSSL`. |
| Cleartext (h2c) | The client knows the server speaks HTTP/2 and sends the connection preface (`PRI * HTTP/2.0`) directly ("prior knowledge"). | `Http2Config::enableH2c`, `true` by default. |
| Cleartext upgrade | Not supported. RFC 9113 §3.1 deprecated `Upgrade: h2c`: such a request is answered over HTTP/1.1, which RFC 9110 §7.8 allows, and the connection stays on HTTP/1.1. | `Http2Config::enableH2cUpgrade` is deprecated and has no effect. |

Clients that attempt the cleartext upgrade (`curl --http2` on an `http://` URL, Java's `java.net.http.HttpClient`) therefore keep using HTTP/1.1, which works but without multiplexing.

## Enable HTTP/2 on a TLS listener

```cpp
#include <chrono>

using namespace std::chrono_literals;

Router router;
router.setPath(http::Method::GET, "/hello", [](const HttpRequestView& req) {
  return req.makeResponse(req.isHttp2() ? "Hello over HTTP/2\n" : "Hello over HTTP/1.1\n");
});

Http2Config http2;
http2.withMaxConcurrentStreams(128).withPingInterval(30s).withEnableH2c(false);

HttpServerConfig config;
config.withPort(8443)
    .withTlsCertKey("/run/tls/fullchain.pem", "/run/tls/private.key")
    .withTlsAlpnProtocols({"h2", "http/1.1"})  // prefer HTTP/2, keep HTTP/1.1 for older clients
    .withHttp2(http2);

SingleHttpServer server(std::move(config), std::move(router));
server.run();
```

Disable h2c (`withEnableH2c(false)`) on public TLS-only listeners where cleartext HTTP/2 is not part of the deployment. The [HTTP/2 example](../../examples/http2.cpp) runs either mode, and the [TLS guide](tls.md#versions-ciphers-and-alpn) explains ALPN negotiation, including strict mode.

## Handlers

HTTP/2 requests reach the same handlers as HTTP/1.1 requests, through the same `HttpRequestView` and the same request pipeline: route patterns, the trailing-slash policy and its redirects, the `HEAD` to `GET` fallback, middleware and its metrics, CORS, the error responses (`404`, `405`, `500`), and the [HTTP to HTTPS redirect](tls.md#http-to-https-redirect) of a plaintext listener apply identically. A handler that needs protocol-specific information can query:

| `HttpRequestView` accessor | HTTP/2 value | HTTP/1.x value |
| --- | --- | --- |
| `isHttp2()` | `true` | `false` |
| `streamId()` | Stream identifier | `0` |
| `scheme()` | `:scheme` pseudo-header | empty |
| `authority()` | `:authority` pseudo-header (equivalent of `Host`) | empty |

Every handler kind works over HTTP/2:

- **Synchronous handlers** answer on their stream.
- **Streaming handlers** (`HttpResponseWriter`) emit DATA frames as the body is written, with flow control and compression.
- **Async handlers** run per stream: a coroutine suspended in `co_await req.deferWork(...)` does not block the other streams of its connection. See [Async handlers](../guides/async-handlers.md#http2).

Request trailers (a trailing `HEADERS` block after the body, RFC 9113 §8.1) are exposed by `trailers()` and `trailerValueOrEmpty()`, exactly like HTTP/1.1 chunked trailers. Responses can carry trailers too, both buffered and streaming; they are sent as a trailing `HEADERS` block.

## Protocol support

| Feature | Status | Notes |
| --- | --- | --- |
| HPACK | Supported | Static and dynamic tables, Huffman coding. The fields whose value differs in most messages (`content-length`, `etag`, `last-modified`, `location`, `set-cookie`, `age`, `if-modified-since`, `if-none-match`) are sent without indexing, like nghttp2 does: each would otherwise be a dynamic table insertion evicting the fields that repeat. Credentials (`authorization`, `proxy-authorization`, and `cookie` values shorter than 20 bytes) are sent as never-indexed literals (RFC 7541 §7.1.3), so that an attacker able to add its own guesses to the requests of a client cannot recognize the right one by its compressed size, and so that intermediaries do not index them either. |
| Stream multiplexing | Supported | Up to `maxConcurrentStreams` concurrent streams per connection. A stream beyond it is reset with `REFUSED_STREAM`, which tells the client that it can retry the request, while the connection and its other streams go on (RFC 9113 §5.1.2). |
| Flow control | Supported | Connection and stream windows, see below. |
| PRIORITY frames | Supported | Optional (`enablePriority`), with a bounded dependency tree (`maxPriorityTreeDepth`). |
| Request and response trailers | Supported | |
| CONNECT | Supported | One tunnel per stream, see [CONNECT tunneling](connect.md#connect-over-http2). |
| PING keepalive | Supported | Optional (`pingInterval`, `pingTimeout`), see [Connection lifetime](#connection-lifetime). |
| Server push | Not supported | Removed from major browsers. `SETTINGS_ENABLE_PUSH` is always advertised as 0, and a received `PUSH_PROMISE` is a connection error. `enablePush` is deprecated and has no effect. |
| Extended CONNECT (RFC 8441) | Not supported | `SETTINGS_ENABLE_CONNECT_PROTOCOL` is not advertised, so WebSocket runs over HTTP/1.1 only. |
| `Upgrade: h2c` | Not supported | See above. |

### Request validation

Request header sections follow the pseudo-header rules of RFC 9113 §8.3. Pseudo-headers must precede regular fields and cannot be repeated; undefined or out-of-context pseudo-headers are rejected. An ordinary request needs exactly one `:method`, `:scheme`, and `:path`, while CONNECT requires `:authority` and forbids `:scheme` and `:path`. An extended CONNECT `:protocol` is recognized and rejected. Repeated regular fields are combined as over HTTP/1.1 (see [Header names](../guides/requests.md#header-names)): list fields are joined, singleton fields such as `authorization` keep their last occurrence, and `content-length` cannot be repeated.

A malformed header section resets only its stream, with `PROTOCOL_ERROR`. An invalid HPACK encoding corrupts the shared compression state, so it remains a connection error (`COMPRESSION_ERROR`), including an integer beyond 32 bits, a string longer than the rest of the block, and a dynamic table size update after the first field of a block (RFC 7541 §4.2). The frames a peer sent on a stream before receiving its reset are ignored (RFC 9113 §5.1): they are not answered with another `RST_STREAM`, and their header blocks are still decoded to keep the compression state in sync. So is a header block on a stream that cannot receive one, such as a second one after the request ended: its stream is reset with `STREAM_CLOSED`, and the other streams go on. The server forgets a closed stream once 16 more streams closed after it: the frames still in flight on it, like the trailers of a request whose body was refused, are ignored as well, instead of failing the connection. A frame other than `HEADERS` and `PRIORITY` on a stream never opened (idle, RFC 9113 §5.1), like a `RST_STREAM` (§6.4), is a connection error (`PROTOCOL_ERROR`), as is a `HEADERS` frame opening a stream below the last one. Pseudo-headers in a trailer block, or a trailer block that does not end the stream, also reset the stream with `PROTOCOL_ERROR`, and trailer bytes count toward the request header size budget.

A request whose `content-length` is not a number, or differs from the total length of its DATA frames, is malformed as well (RFC 9113 §8.1.1): its stream is reset with `PROTOCOL_ERROR`, without response. A body longer than its `content-length` is reset as soon as the excess arrives, a shorter one when the stream ends (after its DATA frames or its trailers), and a request ending its stream with its HEADERS frame may only declare `content-length: 0`. A `content-length` declaring more than `maxBodyBytes` is answered `413` instead (see below). The DATA frames of a CONNECT tunnel are not checked: a CONNECT request has no content.

## Flow control and memory

- Each stream's send window starts from the peer's `SETTINGS_INITIAL_WINDOW_SIZE`, and its receive window from the local `initialWindowSize`. The connection receive window is set separately by `connectionWindowSize` (1 MiB by default).
- The server sends as much as the connection and stream windows allow, and resumes when `WINDOW_UPDATE` frames arrive. A peer lowering its `SETTINGS_INITIAL_WINDOW_SIZE` can make the window of a stream negative (RFC 9113 §6.9.2): the stream then waits until `WINDOW_UPDATE` frames, or a larger setting, bring it above 0, while the connection goes on. Receive credit is returned in batches, once half of a window is consumed, and closed streams receive no stream-level updates. The DATA frames still in flight on a stream the server reset are discarded, but their connection-level credit is returned.
- A request body is kept in memory until the stream ends, bounded by `HttpServerConfig::maxBodyBytes` while it is received: a body exceeding it, or a `content-length` declaring more, is answered `413` right away, then the stream is reset with `NO_ERROR` so that the client stops sending it (RFC 9113 §8.1). A smaller [route limit](../guides/routing.md#per-route-options) is checked once the whole body is received. The body buffer of a request declaring its `content-length` is allocated at once, so that its DATA frames are appended without reallocating it. The allocation is capped by `connectionWindowSize`, beyond which the buffer grows as the frames arrive, so that a client declaring a large body without sending it makes the server allocate no more.
- `maxStreamPendingBytes` (4 MiB by default) bounds the response body and trailer bytes one stream may keep in memory while the peer withholds credit. A larger fixed response is answered `503`; a streaming response that overflows it is reset with `ENHANCE_YOUR_CALM`. The connection-wide `HttpServerConfig::maxOutboundBufferBytes` remains the ceiling across all streams of a connection.
- Files (`HttpResponse::file()`, used by the static file handler) are read as far as the windows allow, in reads of up to 256 KiB each sent as several DATA frames sharing its buffer, but only while the output retained by the connection is under half of `HttpServerConfig::maxOutboundBufferBytes` (2 MiB by default), whatever the credit granted by the client: the other streams keep room for their responses. The file transfers of a connection share this room, each reading at least 16 KiB, and the next reads wait for the output of the connection to be written.
- HEADERS frames and DATA payloads of up to 256 bytes share the connection output buffer, so that a single TLS write encrypts a batch of small responses. Larger DATA and HEADERS blocks keep their own buffers in an ordered fragment queue, across flow-control stalls and partial writes, and cleartext connections gather them with a single `writev` (`WSASend` on Windows). The queued blocks only record their position in the output buffer, which goes on receiving the next frames: a connection keeps one output buffer, reused by its next responses once written, and erases its written part while a slow reader keeps output pending.
- A larger response body is sent from its own storage, without copy, like over HTTP/1.1: the buffer of the response, or that of a body moved in or set by `bodyStatic()` (see [Responses](../guides/responses.md#bodies)). The part of a body exceeding the windows waits for credit there too. Only the DATA frames sent before its last ones copy their payload, as the storage must outlive them until written.
- A header list larger than `maxHeaderListSize` (8 KiB by default; the names and values of its fields, plus 32 bytes per field) resets its stream with `ENHANCE_YOUR_CALM`. Its fields beyond the limit are still decoded, to keep the compression state in sync, but not stored, and the repeated fields of a list are joined once the whole block is decoded: a block referencing a large field of the dynamic table at each of its bytes takes no more memory than the limit to decode.
- Both HPACK dynamic tables of a connection are bounded by `headerTableSize` (4096 bytes by default). The decoder advertises it as `SETTINGS_HEADER_TABLE_SIZE`, and a dynamic table size update above it is a connection error (`COMPRESSION_ERROR`, RFC 7541 §6.3). The encoder keeps at most the smaller of it and the value advertised by the peer, however large. A value below 4096 bytes applies to the decoder once the peer acknowledges the `SETTINGS` (RFC 9113 §4.3.1): its encoder may use the initial 4096 bytes until then.
- The header fields of a request are decoded once, into a storage that the request takes over: they are neither copied nor hashed again. A connection keeps the request objects of up to `maxRecycledRequests` (8 by default) finished streams for its next streams, which reuse the capacity of their header maps and buffers; a buffer larger than 16 KiB, or a map of more than 64 buckets, grown for a large request is released instead. Raise it for connections receiving many request bodies, or running many suspended async handlers, at the same time.
- An idle connection is closed after `HttpServerConfig::keepAliveTimeout`, but a connection with active streams is kept, even when a stream waits for flow-control credit (see [Connection lifetime](#connection-lifetime) to detect dead peers).

## Connection lifetime

The timers below are checked by the periodic maintenance of the server, every `HttpServerConfig::pollInterval` (500 ms by default) at most.

- The client must acknowledge the `SETTINGS` of the server within `settingsTimeout` (5 s by default, 0 disables it): the connection is closed with a `GOAWAY` `SETTINGS_TIMEOUT` otherwise (RFC 9113 §6.5.3).
- With `pingInterval` set (disabled by default), a connection whose streams are open, but on which nothing happened for `pingInterval`, is sent a `PING`. Without its acknowledgement within `pingTimeout` (10 s by default), the client is considered dead, and the connection closed. This bounds the life of a connection whose streams wait for a client that disappeared: such a connection is never idle, so `keepAliveTimeout` does not apply to it. An idle connection is not pinged.
- The stream completing `maxStreamsPerConnection` (1,000,000 by default) sends a graceful `GOAWAY` (`NO_ERROR`) naming it as the last stream: the client opens its next streams on another connection, which a load balancer may route elsewhere. The connection is closed once its streams are answered. The streams the client opened meanwhile, above the last one, are ignored (RFC 9113 §6.8), and retried by the client on its new connection; their header blocks are still decoded to keep the compression state in sync.

- A [drain](../guides/server-lifecycle.md#stopping) (`beginDrain()`, or a signal) sends the same graceful `GOAWAY` to every HTTP/2 connection, at the next maintenance tick: like an HTTP/1.1 connection answering its current request with `Connection: close`, it serves its open streams, then closes, and an idle one closes right away. The drain then ends without waiting for its deadline.

The connections closed by a timeout are counted by the `aeronet.connections.closed_for_http2_settings_timeout` and `aeronet.connections.closed_for_http2_ping_timeout` counters (see [Observability](../operations/observability.md)).

## Configuration

`Http2Config` is passed to `HttpServerConfig::withHttp2()`. The settings that matter most in production are:

| Setting | Default | Guidance |
| --- | --- | --- |
| `maxConcurrentStreams` | 100 | Bounds the parallelism, and the memory, of one client. |
| `initialWindowSize`, `connectionWindowSize` | 1 MiB, 1 MiB | What the peer may send on a stream, and on the whole connection, before receiving credit: about the bytes one upload can transfer per round trip. Request bodies are kept whole in memory whatever the windows, but a [CONNECT tunnel](connect.md#connect-over-http2) to a slow target may hold up to its stream window. |
| `maxStreamPendingBytes` | 4 MiB | Response memory one stream may retain while waiting for credit. |
| `pingInterval`, `pingTimeout` | disabled, 10 s | Detect dead peers on the connections whose streams are open, see [Connection lifetime](#connection-lifetime). |
| `maxStreamsPerConnection` | 1,000,000 | Streams served before a graceful `GOAWAY`, to rebalance long-lived connections. |
| `sendContentLengthHeader` | `true` | Add `content-length` to responses built with `makeResponse()`. Optional in HTTP/2, but helps some clients and intermediaries. |

The [server configuration reference](../reference/server-configuration.md#http2-configuration) lists every field with its default.

!!! warning
    Flow-control windows, concurrent stream limits, and pending byte caps are production settings. Size them for your workload and memory budget rather than reusing benchmark-oriented values.

## Try it

```bash
# TLS with ALPN negotiation
curl -k --http2 https://localhost:8443/hello

# Cleartext h2c with prior knowledge
curl --http2-prior-knowledge http://localhost:8080/hello

# Upgrade: h2c is ignored: answered over HTTP/1.1
curl --http2 http://localhost:8080/hello
```

`h2load` (from nghttp2) generates multiplexed load: `h2load -n 100000 -c 10 -m 32 https://localhost:8443/hello`.

## Client side

`HttpClient` speaks HTTP/2 too, reusing the same HPACK and frame codecs: see [HTTP client](../modules/http-client.md#http2).

## Tests

- Connection-level behavior, flow control, malformed field sections, frames on reset streams, frames on forgotten and idle streams (`FieldBlocksOnAStreamResetThenPrunedAreDecodedAndIgnored`, `ContinuationOfAStreamPrunedMeanwhileIsDecoded`, `ClientIgnoresTheResponseOfAForgottenStreamAndRejectsTheOthers`, `FramesOnIdleStreamsAreConnectionErrors`), new streams after a GOAWAY (`NewStreamsAfterTheGoAwayOfThisEndpointAreDecodedAndIgnored`), the connection timers (`MaxStreamsPerConnectionSendsAGracefulGoAway`, `UnacknowledgedSettingsTimeOut`, `PingKeepAliveDetectsADeadPeer`), the output queue (`OutputBufferIsKeptAcrossOwnedDataBlocks`, `OutputQueueStaysBoundedWhenNeverFullyDrained`, `WrittenOutputBufferBytesAreErasedOnceHalfOfIt`), header blocks on half-closed streams (`FieldBlockOnAHalfClosedRemoteStreamIsDecodedBeforeTheReset`, `UndecodableFieldBlockOnAHalfClosedRemoteStreamIsAConnectionError`), the HPACK dynamic table limits (`DynamicTableSizeUpdateAboveHeaderTableSizeIsCompressionError`, `SmallerHeaderTableSizeAppliesOnSettingsAck`, `EncoderDynamicTableIsCappedByHeaderTableSize`, `LargerPeerHeaderTableSizeSendsNoSizeUpdate`), a stream window made negative by `SETTINGS_INITIAL_WINDOW_SIZE` (`SettingsInitialWindowSizeDecreaseCanMakeStreamWindowNegative`), and the streams above `maxConcurrentStreams` (`HeadersAboveMaxConcurrentStreamsRefuseTheStream`): [http2-connection_test.cpp](../../aeronet/http2/test/http2-connection_test.cpp), and [http2-stream_test.cpp](../../aeronet/http2/test/http2-stream_test.cpp) (`UpdateInitialWindowSizeDecreaseCanMakeSendWindowNegative`, `UpdateInitialWindowSizeToZeroAfterConsumingTheLargestWindow`).
- Request processing, pseudo-header validation, trailers, request bodies and their limits, recycled requests (`RecycledRequestStartsFromAFreshState`, `MoreConcurrentRequestsThanRecycledOnes`, `RecycledRequestReleasesItsLargeBuffers`), response bodies sent from their storage (`CapturedBodyIsSentWithoutCopy`, `CapturedBodyLargerThanWindowIsSentFromItsStorage`), response bodies and files waiting for flow-control credit (`InitialWindowSizeDecreaseDuringResponseWaitsForCredit`), file read-ahead (`FileReadAheadIsBoundedWhateverTheFlowControlCredit`, `FileSendsShareTheReadAheadRoom`), tunnel window updates, per-route request timeouts, the GOAWAY of a drain (`InitiateGoAwayClosesTheConnectionOnceItsStreamsAreAnswered`): [http2-protocol-handler_test.cpp](../../aeronet/server/test/http2-protocol-handler_test.cpp).
- HPACK, including the indexing mode of each field (`HpackEncoder.CredentialsAreNeverIndexed`), the decoded headers taken over by their owner (`HpackDecoder.TakenOverHeadersOutliveTheNextDecodes`, `HpackDecoder.StaticTableFieldsAreNotCopied`), repeated fields (`HpackDecoder.OverrideHeaderKeepsLastValue`, `HpackDecoder.DuplicateWithEmptyValueAddsNoSeparator`, `HpackDecoder.ManyRepeatedFieldsAreJoinedOnceInOrder`), the header list size limit (`HpackDecoder.HeaderListBeyondTheLimitIsNotStored`, `HpackDecoder.HeaderListAtTheLimitIsAccepted`), the dynamic table size limit (`HpackDecoder.DynamicTableSizeUpdateAboveTheLimitIsACompressionError`, `HpackDecoder.SetMaxDynamicTableSizeBoundsTheSizeUpdates`, `HpackDecoder.DynamicTableSizeUpdateAfterAFieldIsACompressionError`, `HpackEncoder.SetMaxDynamicTableSizeSignalsOnlyAChange`), and integer and string length limits (`HpackDecoder.IntegerNotFittingIn32BitsIsAnError`, `HpackDecoder.StringLongerThanTheBlockIsAnError`): [hpack_test.cpp](../../aeronet/http2/test/hpack_test.cpp).
- End-to-end over TLS and h2c, including the connection lifetime (`ConnectionClosesAfterTheStreamsOfItsBudget`, `UnacknowledgedServerSettingsCloseTheConnection`, `PingKeepAliveClosesTheConnectionOfADeadPeer`, `DrainSendsAGoAwayAndClosesTheConnectionsOnceTheirStreamsAreAnswered`): [tests/http2-core_test.cpp](../../tests/http2-core_test.cpp), and CONNECT: [tests/http2-connect_test.cpp](../../tests/http2-connect_test.cpp).
- The same request pipeline scenarios over HTTP/1.1 and h2c (middleware, metrics, CORS, redirects, handler errors, streaming and async handlers): [tests/http-protocol-parity_test.cpp](../../tests/http-protocol-parity_test.cpp).
