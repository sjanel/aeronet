# WebSocket

aeronet implements the WebSocket protocol ([RFC 6455](https://www.rfc-editor.org/rfc/rfc6455)) on top of its HTTP/1.1 server: a route accepts the `Upgrade: websocket` handshake, then the connection carries full-duplex text and binary messages, ping/pong heartbeats, and a close handshake. The optional `permessage-deflate` extension ([RFC 7692](https://www.rfc-editor.org/rfc/rfc7692)) compresses messages.

WebSocket support is enabled by the `AERONET_ENABLE_WEBSOCKET` CMake option, which defaults to `ON` whenever the server is built. Compression additionally requires zlib (`AERONET_ENABLE_ZLIB`).

## A first endpoint

`Router::setWebSocket()` registers an endpoint on a path. The simplest endpoint shares one set of callbacks between all its connections:

```cpp
#include <span>

Router router;
router.setWebSocket("/log", WebSocketEndpoint::WithCallbacks(websocket::WebSocketCallbacks{
                                .onMessage = [](std::span<const std::byte> payload, bool isBinary) {
                                  // Process one complete message (fragments already reassembled).
                                },
                            }));
```

Shared callbacks cannot answer a message, because they do not know which connection it came from. To send data, create one handler per connection with a factory. The factory receives the upgrade request and returns the handler that will own the connection:

```cpp
#include <memory>
#include <span>
#include <string_view>

Router router;
router.setWebSocket("/echo", WebSocketEndpoint::WithFactory([](const HttpRequestView& /*upgradeRequest*/) {
  auto handler = std::make_unique<websocket::WebSocketHandler>();
  websocket::WebSocketHandler* connection = handler.get();  // valid as long as the connection lives

  handler->setCallbacks(websocket::WebSocketCallbacks{
      .onMessage =
          [connection](std::span<const std::byte> payload, bool isBinary) {
            if (isBinary) {
              connection->sendBinary(payload);
            } else {
              connection->sendText(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
            }
          },
      .onClose = [](websocket::CloseCode code, std::string_view reason) {
        // The close handshake is answered automatically.
      },
  });
  return handler;
}));

SingleHttpServer server(HttpServerConfig{}.withPort(8080), std::move(router));
server.run();
```

The handler is destroyed with its connection, so the pointer captured by its own callbacks never dangles. The [WebSocket echo example](../../examples/websocket-echo.cpp) serves a small browser client on top of this pattern.

## Defining endpoints

`WebSocketEndpoint` holds the configuration, the supported subprotocols, and the handler factory of a path. Its static helpers cover the common combinations:

| Helper | Use it when |
| --- | --- |
| `WithCallbacks(callbacks)` | All connections share stateless callbacks and use the default configuration. |
| `WithConfigAndCallbacks(config, callbacks)` | Same, with custom limits or compression settings. |
| `WithProtocolsAndCallbacks(protocols, callbacks)` | The endpoint negotiates a subprotocol. |
| `WithFullConfig(config, protocols, callbacks)` | Both of the above. |
| `WithFactory(factory)` | Each connection needs its own state, or must send messages. |

The factory receives the upgrade request after the request middleware ran, with its path parameters. It can refuse the connection by returning `nullptr`: the server then answers `403 Forbidden`. An exception escaping the factory is logged and answered `500 Internal Server Error`. The factory runs on the event-loop thread, so keep it fast. `RouteGroup::setWebSocket()` registers an endpoint under a group prefix like `setPath()` does.

A path may carry both a WebSocket endpoint and ordinary handlers: a `GET` request with `Upgrade: websocket` goes to the endpoint, and other requests are routed to the handlers registered for their method.

## Callbacks

All callbacks run on the event-loop thread of the connection.

| Callback | Invoked when |
| --- | --- |
| `onMessage(payload, isBinary)` | A complete message arrived. Fragments are reassembled and decompressed, and a text message is guaranteed to be valid UTF-8. |
| `onPing(payload)` | A ping arrived. The pong is sent automatically, the callback is informational. |
| `onPong(payload)` | A pong arrived, typically in answer to `sendPing()`. |
| `onClose(code, reason)` | The peer sent a Close frame. The server answers it automatically. |
| `onError(code, message)` | The peer violated the protocol or exceeded a limit. The server then sends a Close frame with `code` and closes the connection. |

The payload span is valid only during the callback: copy it to keep it.

Text messages are validated before `onMessage` with SIMD instructions, AVX2 or SSSE3 on x86 (detected at runtime) and NEON on ARM, at the same speed whatever their script: about 25 GB/s with AVX2. A message that is not valid UTF-8 (RFC 3629: no overlong encoding, surrogate, code point above U+10FFFF or truncated sequence) closes the connection with `InvalidPayloadData` (1007).

## Sending messages

`WebSocketHandler` sends with `sendText()`, `sendBinary()`, `sendPing()`, `sendPong()`, and `sendClose(code, reason)`. Each call queues a frame on the connection and returns `false` once the close handshake has started. A message is sent as a single frame. When compression was negotiated, a message of at least `DeflateConfig::minCompressSize` bytes is sent compressed, unless compression does not make it smaller.

Sending from `onMessage` a payload of at least 4 KiB that it received (an echo, or a part of the message) does not copy it: only the frame header is queued, and the server writes the payload from its receive buffer right after the callbacks of this input, copying only what the socket did not accept.

These methods are not thread-safe: call them from a callback of the same connection. There is currently no way to push a message to a connection from another thread.

## Upgrade handshake

The server accepts an upgrade when the request:

1. uses the `GET` method,
2. carries `Upgrade: websocket` and `Connection: Upgrade`,
3. carries `Sec-WebSocket-Version: 13`,
4. carries a valid `Sec-WebSocket-Key` (16 random bytes in base64, 24 characters).

A `GET` request on the endpoint's path with an `Upgrade: websocket` header goes through the same steps as other requests before it is upgraded:

1. The global and route [request middleware](../guides/middleware.md) run: a middleware can authenticate the request, and refuse it by answering it (`401 Unauthorized`, for instance).
2. The `Origin` header is checked, see below. A refused origin receives `403 Forbidden`.
3. The handshake headers are validated: when they fail the checks above, the answer is `400 Bad Request`.
4. The endpoint's factory creates the connection's handler, and may refuse the connection.

On success, the server answers `101 Switching Protocols` with the computed `Sec-WebSocket-Accept`, and the connection switches to the WebSocket protocol. Frames received along with the upgrade request (from a client that did not wait for the `101` response) are processed right away. Response middleware does not apply to the `101` response. Any other request is routed like a normal HTTP request.

**Origin checks.** Browsers do not apply CORS to WebSocket connections: a page of any site can open one to the server, and the browser attaches the server's cookies to the upgrade request. To prevent this cross-site WebSocket hijacking, the server checks the `Origin` header that browsers always send:

- Without [CORS policy](../guides/cors.md) for the route, only the server's own pages may connect: the host and port of `Origin` must be those of the `Host` header (the default port of the scheme may be omitted on either side). An opaque origin (`null`) is refused.
- With a CORS policy (the route's or the router's default), the policy decides: an origin it does not allow is refused, and an allowed origin is accepted even from another site.
- A request without `Origin` comes from a client that is not a browser, and is accepted.

```cpp
CorsPolicy cors;
cors.allowOrigin("https://app.example.com");  // the site hosting the pages that open the connection

Router router;
router.setWebSocket("/ws", WebSocketEndpoint::WithCallbacks(websocket::WebSocketCallbacks{})).cors(std::move(cors));
```

!!! warning "Behind a reverse proxy"
    A proxy that sets the `Host` header of the requests it forwards to the address of the upstream, as nginx does by default (`proxy_set_header Host $proxy_host`), makes the same-origin check fail for every browser: `Origin` names the public site, while `Host` names the upstream. Every browser upgrade is then answered `403`. Either attach a CORS policy listing the allowed origins to the route, or have the proxy forward the `Host` header sent by the browser (`proxy_set_header Host $http_host;` with nginx, which keeps its port).

**Subprotocols.** When the client sends `Sec-WebSocket-Protocol`, the server selects the first of the endpoint's `supportedProtocols` (in the endpoint's preference order) that the client offered, compared case-insensitively. When none matches, the handshake succeeds without a subprotocol, and the client decides whether to continue.

## Configuration

`websocket::WebSocketConfig` sets the limits of an endpoint's connections:

| Field | Default | Effect |
| --- | --- | --- |
| `maxMessageSize` | 64 MiB | Maximum size of a reassembled message, decompressed. `0` disables the limit. A larger message closes the connection with `MessageTooBig` (1009), or `InvalidPayloadData` (1007) when it is compressed. |
| `maxFrameSize` | 16 MiB | Maximum payload of a single frame. A larger frame closes the connection with `MessageTooBig`. |
| `idleTimeout` | 60 s | A connection on which nothing was received for this long is closed. `0` disables it. See [Idle connections](#idle-connections). |
| `autoPing` | `true` | Send a Ping after half of `idleTimeout` without received data, so that live peers are never closed. |
| `closeTimeout` | 5 s | Time to wait for the peer's Close frame after sending one, before closing the connection. See [Closing connections](#closing-connections). |
| `deflateConfig` | see below | `permessage-deflate` settings. |

```cpp
#include <chrono>

websocket::WebSocketConfig config;
config.maxMessageSize = 1024 * 1024;
config.maxFrameSize = 256 * 1024;

Router router;
router.setWebSocket("/ws", WebSocketEndpoint::WithConfigAndCallbacks(config, websocket::WebSocketCallbacks{}));
```

Frames may arrive split across any number of reads, or several in one read: the handler consumes complete frames only, and keeps the bytes of an incomplete trailing frame in the connection buffer without copying them. A connection without such pending bytes reads into the input buffer of the previous read of its server thread, still in the CPU caches, rather than into its own: with many connections exchanging large messages, they would otherwise be received into cold memory. Client frames must be masked; an unmasked frame is a protocol error. Unmasking uses SIMD instructions (AVX2 or SSE2 on x86, NEON on ARM) with a scalar fallback.

## Compression (permessage-deflate)

When the client offers `permessage-deflate` and the endpoint enables it, the server negotiates the extension and compresses and decompresses messages transparently: callbacks always see uncompressed payloads.

```cpp
websocket::WebSocketConfig config;
config.deflateConfig.compressionLevel = 3;  // faster than the default 6
config.deflateConfig.minCompressSize = 1024;
config.deflateConfig.serverNoContextTakeover = true;  // less memory per connection, lower ratio

Router router;
router.setWebSocket("/feed", WebSocketEndpoint::WithConfigAndCallbacks(config, websocket::WebSocketCallbacks{}));
```

| `DeflateConfig` field | Default | Meaning |
| --- | --- | --- |
| `enabled` | `true` with zlib | Negotiate the extension when the client offers it. |
| `compressionLevel` | 6 | zlib level, 0 to 9. |
| `serverMaxWindowBits` | 15 | LZ77 window of the server's compressor, 8 to 15 (2^N bytes). |
| `clientMaxWindowBits` | 15 | LZ77 window of the client's compressor, 8 to 15. Only applied to clients offering `client_max_window_bits`. |
| `serverNoContextTakeover` | `false` | Reset the server's compression context after each message: lower ratio, but no compression stream per connection (see below). |
| `clientNoContextTakeover` | `false` | Ask the client to reset its context after each message: no decompression stream per connection (see below). |
| `minCompressSize` | 256 bytes | Smaller messages are sent uncompressed. |

The server accepts the first acceptable offer of the client, and each side compresses with the window negotiated for it. zlib cannot produce raw DEFLATE data with a 256-byte window: when 8 is negotiated, aeronet compresses with 9 bits, whose matches never refer more than 250 bytes back, so the peer decodes them with its 256-byte window. A client that does not offer `client_max_window_bits` keeps a 32 KiB window: RFC 7692 forbids limiting it. Messages are raw DEFLATE data, with no zlib header nor checksum and with the trailing `00 00 ff ff` removed (RFC 7692 §7.2.1), and interoperate with browsers and standard WebSocket libraries.

A direction negotiated with context takeover (the default) continues the same DEFLATE stream from message to message, so that a message can refer to the previous ones: its connection keeps a zlib stream for it, allocated by its first message (about 300 KB to compress, 40 KB to decompress). A direction negotiated without context takeover (`server_no_context_takeover` and `client_no_context_takeover`, requested by either side) starts each message afresh: its connections share a single stream per server thread, which uses no memory per connection and stays in the CPU caches. This is much faster for many connections exchanging small messages, the compression stream being reset for each one.

When a factory returns a handler without compression and the client negotiated it, the server enables it on that handler (`WebSocketHandler::enableCompression()`), keeps its callbacks, and gives it the streams shared by its server thread.

## Closing connections

When the peer sends a Close frame, the server invokes `onClose()`, answers with its own Close frame, then closes the TCP connection. Closing first leaves the `TIME_WAIT` state on the server rather than on the client, as RFC 6455 §7.1.1 recommends.

The server initiates the close itself when the peer violates the protocol:

| Close code | Sent when |
| --- | --- |
| `ProtocolError` (1002) | Malformed frame, unmasked client frame, unexpected or missing continuation frame. |
| `InvalidPayloadData` (1007) | Text message that is not valid UTF-8, or a compressed message that fails to decompress. |
| `MessageTooBig` (1009) | Frame larger than `maxFrameSize`, or message larger than `maxMessageSize`. |

An application closes a connection with `sendClose(code, reason)`. `websocket::CloseCode` lists the standard codes, from `Normal` (1000) to `TLSHandshake` (1015). `NoStatusReceived` (1005), `AbnormalClosure` (1006) and `TLSHandshake` (1015) are reserved for reporting and are never sent in a frame.

After sending a Close frame, from `sendClose()` or after a protocol error, the server waits `closeTimeout` (5 seconds by default) for the peer's Close frame, then closes the connection.

## Idle connections

`HttpServerConfig::keepAliveTimeout` bounds the idleness between HTTP requests: it does not apply to an upgraded connection, which has its own `idleTimeout` (60 seconds by default). A connection on which nothing was received for `idleTimeout` is closed, without Close frame: the peer is most likely gone (a closed laptop, a lost network).

Browsers do not send pings on their own, and an application may legitimately exchange no message for minutes. With `autoPing` (the default), the server sends a Ping once the connection has been idle for half of `idleTimeout`. Browsers and WebSocket libraries answer it with a Pong, which restarts the idle period: live connections stay open, while dead ones are closed after `idleTimeout`. These Pings also keep the connection alive through proxies and NATs that drop idle connections (nginx closes a proxied connection idle for 60 seconds by default).

```cpp
#include <chrono>

websocket::WebSocketConfig config;
config.idleTimeout = std::chrono::minutes{5};  // close a connection that stays silent for 5 minutes
config.autoPing = false;                       // the application sends its own heartbeats

Router router;
router.setWebSocket("/feed", WebSocketEndpoint::WithConfigAndCallbacks(config, websocket::WebSocketCallbacks{}));
```

The timeouts are checked by the server's maintenance timer, so they are accurate to `HttpServerConfig::pollInterval`. Each connection has a single entry in the deadline queue that also drives the keep-alive timeout, updated only when its next check comes closer: a busy connection costs nothing per message.

## Limitations

- WebSocket runs over HTTP/1.1 only. WebSocket over HTTP/2 (RFC 8441, extended CONNECT) is not supported.
- Messages can only be sent from the event-loop thread of their connection, from a callback.
- Messages are delivered whole: there is no streaming API for large messages, which are buffered up to `maxMessageSize`.
- aeronet provides no WebSocket client.

## Tests

- Handshake, middleware, origin checks, factory refusal, messaging, large echoes (also when the client does not read), compression interoperability and connections sharing the streams of a server thread, close handshake, and idle and close timeouts against a real socket: [tests/websocket-integration_test.cpp](../../tests/websocket-integration_test.cpp).
- Frame parsing, masking, and SIMD unmasking: [websocket-frame_test.cpp](../../aeronet/websocket/test/websocket-frame_test.cpp).
- Message reassembly, control frames, close handshake, split frames, payloads sent from the input (`WebSocketInputPayloadTest`), and timeout computation: [websocket-handler_test.cpp](../../aeronet/websocket/test/websocket-handler_test.cpp).
- UTF-8 validation by each kernel (scalar, SSSE3, AVX2, NEON) against a reference validator, for every byte pair and every sample at every block offset: [websocket-utf8_test.cpp](../../aeronet/websocket/test/websocket-utf8_test.cpp).
- Upgrade validation and subprotocol negotiation: [websocket-upgrade_test.cpp](../../aeronet/websocket/test/websocket-upgrade_test.cpp).
- permessage-deflate negotiation and interoperability, context takeover across messages, streams shared without context takeover (`WebSocketSharedDeflateStreamsTest`): [websocket-deflate_test.cpp](../../aeronet/websocket/test/websocket-deflate_test.cpp) and [websocket-compress_test.cpp](../../aeronet/websocket/test/websocket-compress_test.cpp).
