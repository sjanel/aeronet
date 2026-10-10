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

The factory must return a non-null handler. It runs on the event-loop thread, so keep it fast. `RouteGroup::setWebSocket()` registers an endpoint under a group prefix like `setPath()` does.

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

## Sending messages

`WebSocketHandler` sends with `sendText()`, `sendBinary()`, `sendPing()`, `sendPong()`, and `sendClose(code, reason)`. Each call queues a frame on the connection and returns `false` once the close handshake has started. A message is sent as a single frame. When compression was negotiated, a message of at least `DeflateConfig::minCompressSize` bytes is sent compressed, unless compression does not make it smaller.

These methods are not thread-safe: call them from a callback of the same connection. There is currently no way to push a message to a connection from another thread.

## Upgrade handshake

The server accepts an upgrade when the request:

1. uses the `GET` method,
2. carries `Upgrade: websocket` and `Connection: Upgrade`,
3. carries `Sec-WebSocket-Version: 13`,
4. carries a valid `Sec-WebSocket-Key` (16 random bytes in base64, 24 characters).

On success, it answers `101 Switching Protocols` with the computed `Sec-WebSocket-Accept`, and the connection switches to the WebSocket protocol. A `GET` request with an `Upgrade: websocket` header that fails these checks receives `400 Bad Request`. Any other request is routed like a normal HTTP request.

**Subprotocols.** When the client sends `Sec-WebSocket-Protocol`, the server selects the first of the endpoint's `supportedProtocols` (in the endpoint's preference order) that the client offered, compared case-insensitively. When none matches, the handshake succeeds without a subprotocol, and the client decides whether to continue.

!!! warning
    The upgrade bypasses the request middleware and CORS policies, and the factory cannot refuse it. The server does not check the `Origin` header either. Do not rely on cookies alone to authenticate a WebSocket connection opened by a browser (cross-site WebSocket hijacking): authenticate the first message, or put an authenticating proxy in front of the server.

## Configuration

`websocket::WebSocketConfig` sets the limits of an endpoint's connections:

| Field | Default | Effect |
| --- | --- | --- |
| `maxMessageSize` | 64 MiB | Maximum size of a reassembled message. `0` disables the limit. A larger message closes the connection with `MessageTooBig` (1009). |
| `maxFrameSize` | 16 MiB | Maximum payload of a single frame. A larger frame closes the connection with `MessageTooBig`. |
| `closeTimeout` | 5 s | Time to wait for the peer's Close frame after sending one. See [Closing connections](#closing-connections). |
| `deflateConfig` | see below | `permessage-deflate` settings. |

```cpp
#include <chrono>

websocket::WebSocketConfig config;
config.maxMessageSize = 1024 * 1024;
config.maxFrameSize = 256 * 1024;

Router router;
router.setWebSocket("/ws", WebSocketEndpoint::WithConfigAndCallbacks(config, websocket::WebSocketCallbacks{}));
```

Frames may arrive split across any number of reads, or several in one read: the handler consumes complete frames only, and keeps the bytes of an incomplete trailing frame in the connection buffer without copying them. Client frames must be masked; an unmasked frame is a protocol error. Unmasking uses SIMD instructions (AVX2 or SSE2 on x86, NEON on ARM) with a scalar fallback.

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
| `serverMaxWindowBits` | 15 | LZ77 window of the server's compressor, 9 to 15 (2^N bytes). |
| `clientMaxWindowBits` | 15 | LZ77 window of the client's compressor, 8 to 15. |
| `serverNoContextTakeover` | `false` | Reset the server's compression context after each message: less memory, lower ratio. |
| `clientNoContextTakeover` | `false` | Ask the client to reset its context after each message. |
| `minCompressSize` | 256 bytes | Smaller messages are sent uncompressed. |

The server accepts the first acceptable offer of the client, and each side compresses with the window negotiated for it. zlib cannot produce raw DEFLATE data with a 256-byte window, so an offer requiring `server_max_window_bits=8` is declined and the connection proceeds without compression. Messages are raw DEFLATE data, with no zlib header nor checksum and with the trailing `00 00 ff ff` removed (RFC 7692 §7.2.1), and interoperate with browsers and standard WebSocket libraries.

When a factory returns a handler without compression and the client negotiated it, the server enables it on that handler (`WebSocketHandler::enableCompression()`) and keeps its callbacks.

## Closing connections

When the peer sends a Close frame, the server invokes `onClose()`, answers with its own Close frame, then closes the TCP connection. Closing first leaves the `TIME_WAIT` state on the server rather than on the client, as RFC 6455 §7.1.1 recommends.

The server initiates the close itself when the peer violates the protocol:

| Close code | Sent when |
| --- | --- |
| `ProtocolError` (1002) | Malformed frame, unmasked client frame, unexpected or missing continuation frame. |
| `InvalidPayloadData` (1007) | Text message that is not valid UTF-8, or a compressed message that fails to decompress. |
| `MessageTooBig` (1009) | Frame larger than `maxFrameSize`, or message larger than `maxMessageSize`. |

An application closes a connection with `sendClose(code, reason)`. `websocket::CloseCode` lists the standard codes, from `Normal` (1000) to `TLSHandshake` (1015). `NoStatusReceived` (1005), `AbnormalClosure` (1006) and `TLSHandshake` (1015) are reserved for reporting and are never sent in a frame.

An upgraded connection stays subject to the server's idle timeout, `HttpServerConfig::keepAliveTimeout` (5 seconds by default): a WebSocket connection on which no byte flows for that long is closed. Browsers do not send pings on their own, so either raise `keepAliveTimeout` for servers with long-lived connections, or have the application exchange periodic messages. The same timeout bounds the wait for the peer's Close frame. The server does not enforce `closeTimeout` yet: `WebSocketHandler::hasCloseTimedOut()` only reports whether it elapsed.

## Limitations

- WebSocket runs over HTTP/1.1 only. WebSocket over HTTP/2 (RFC 8441, extended CONNECT) is not supported.
- The upgrade bypasses request middleware and cannot be refused by the factory (see the warning in [Upgrade handshake](#upgrade-handshake)).
- Messages can only be sent from the event-loop thread of their connection, from a callback.
- The server does not send pings by itself, and does not enforce `closeTimeout`.
- Messages are delivered whole: there is no streaming API for large messages, which are buffered up to `maxMessageSize`.
- aeronet provides no WebSocket client.

## Tests

- Handshake, messaging, compression interoperability, and close handshake against a real socket: [tests/websocket-integration_test.cpp](../../tests/websocket-integration_test.cpp).
- Frame parsing, masking, and SIMD unmasking: [websocket-frame_test.cpp](../../aeronet/websocket/test/websocket-frame_test.cpp).
- Message reassembly, control frames, close handshake, and split frames: [websocket-handler_test.cpp](../../aeronet/websocket/test/websocket-handler_test.cpp).
- Upgrade validation and subprotocol negotiation: [websocket-upgrade_test.cpp](../../aeronet/websocket/test/websocket-upgrade_test.cpp).
- permessage-deflate negotiation and interoperability: [websocket-deflate_test.cpp](../../aeronet/websocket/test/websocket-deflate_test.cpp) and [websocket-compress_test.cpp](../../aeronet/websocket/test/websocket-compress_test.cpp).
