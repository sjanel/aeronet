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

HTTP/2 requests reach the same handlers as HTTP/1.1 requests, through the same `HttpRequestView`. Route patterns, the trailing-slash policy, the `HEAD` to `GET` fallback, middleware, and CORS apply identically. A handler that needs protocol-specific information can query:

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
| HPACK | Supported | Static and dynamic tables, Huffman coding. |
| Stream multiplexing | Supported | Up to `maxConcurrentStreams` concurrent streams per connection. |
| Flow control | Supported | Connection and stream windows, see below. |
| PRIORITY frames | Supported | Optional (`enablePriority`), with a bounded dependency tree (`maxPriorityTreeDepth`). |
| Request and response trailers | Supported | |
| CONNECT | Supported | One tunnel per stream, see [CONNECT tunneling](connect.md#connect-over-http2). |
| PING keepalive | Supported | Optional, with `pingInterval` and `pingTimeout`. |
| Server push | Not supported | Removed from major browsers. `SETTINGS_ENABLE_PUSH` is always advertised as 0, and a received `PUSH_PROMISE` is a connection error. `enablePush` is deprecated and has no effect. |
| Extended CONNECT (RFC 8441) | Not supported | `SETTINGS_ENABLE_CONNECT_PROTOCOL` is not advertised, so WebSocket runs over HTTP/1.1 only. |
| `Upgrade: h2c` | Not supported | See above. |

### Request validation

Request header sections follow the pseudo-header rules of RFC 9113 §8.3. Pseudo-headers must precede regular fields and cannot be repeated; undefined or out-of-context pseudo-headers are rejected. An ordinary request needs exactly one `:method`, `:scheme`, and `:path`, while CONNECT requires `:authority` and forbids `:scheme` and `:path`. An extended CONNECT `:protocol` is recognized and rejected.

A malformed header section resets only its stream, with `PROTOCOL_ERROR`. An invalid HPACK encoding corrupts the shared compression state, so it remains a connection error (`COMPRESSION_ERROR`). Pseudo-headers in a trailer block, or a trailer block that does not end the stream, also reset the stream with `PROTOCOL_ERROR`, and trailer bytes count toward the request header size budget.

## Flow control and memory

- Each stream's send window starts from the peer's `SETTINGS_INITIAL_WINDOW_SIZE`, and its receive window from the local `initialWindowSize`. The connection receive window is set separately by `connectionWindowSize` (1 MiB by default).
- The server sends as much as the connection and stream windows allow, and resumes when `WINDOW_UPDATE` frames arrive. Receive credit is returned in batches, once half of a window is consumed, and closed streams receive no stream-level updates.
- `maxStreamPendingBytes` (4 MiB by default) bounds the response body and trailer bytes one stream may keep in memory while the peer withholds credit. A larger fixed response is answered `503`; a streaming response that overflows it is reset with `ENHANCE_YOUR_CALM`. The connection-wide `HttpServerConfig::maxOutboundBufferBytes` remains the ceiling across all streams of a connection.
- Files (`HttpResponse::file()`, used by the static file handler) are read in bounded chunks into DATA frames, as the windows allow, and never loaded entirely in memory.
- HEADERS frames and DATA payloads of up to 256 bytes share the connection output buffer, so that a single TLS write encrypts a batch of small responses. Larger DATA and HEADERS blocks keep their own buffers in an ordered fragment queue, across flow-control stalls and partial writes, and cleartext connections gather them with a single `writev` (`WSASend` on Windows).
- An idle connection is closed after `HttpServerConfig::keepAliveTimeout`, but a connection with active streams is kept, even when a stream waits for flow-control credit.

## Configuration

`Http2Config` is passed to `HttpServerConfig::withHttp2()`. The settings that matter most in production are:

| Setting | Default | Guidance |
| --- | --- | --- |
| `maxConcurrentStreams` | 100 | Bounds the parallelism, and the memory, of one client. |
| `initialWindowSize`, `connectionWindowSize` | 64 KiB, 1 MiB | Larger windows speed up large uploads on high-latency links, at the cost of memory per connection. |
| `maxStreamPendingBytes` | 4 MiB | Response memory one stream may retain while waiting for credit. |
| `pingInterval`, `pingTimeout` | disabled, 10 s | Detect dead peers on long-lived connections. |
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

- Connection-level behavior, flow control, and malformed field sections: [http2-connection_test.cpp](../../aeronet/http2/test/http2-connection_test.cpp).
- Request processing, pseudo-header validation, and trailers: [http2-protocol-handler_test.cpp](../../aeronet/http2/test/http2-protocol-handler_test.cpp).
- HPACK: [hpack_test.cpp](../../aeronet/http2/test/hpack_test.cpp).
- End-to-end over TLS and h2c: [tests/http2-core_test.cpp](../../tests/http2-core_test.cpp), and CONNECT: [tests/http2-connect_test.cpp](../../tests/http2-connect_test.cpp).
