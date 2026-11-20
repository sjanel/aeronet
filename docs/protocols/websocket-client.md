# WebSocket client

`WebSocketClient` opens WebSocket connections ([RFC 6455](https://www.rfc-editor.org/rfc/rfc6455)) to servers, such as market data feeds or browser automation endpoints, and serves any number of them from the thread calling its `poll()` method. Messages compressed with `permessage-deflate` ([RFC 7692](https://www.rfc-editor.org/rfc/rfc7692)) are supported.

The client is built when both the `AERONET_ENABLE_HTTP_CLIENT` and `AERONET_ENABLE_WEBSOCKET` CMake options are `ON` (the defaults). `wss://` URLs require `AERONET_ENABLE_OPENSSL`, and compression requires `AERONET_ENABLE_ZLIB`. Include `<aeronet/websocket-client.hpp>`, or the `<aeronet/aeronet-client.hpp>` umbrella header. The frame protocol is the one of the [server endpoints](websocket.md), in its client mode.

## A first connection

`connect()` performs the opening handshake, then `poll()` delivers the messages to the callbacks:

```cpp
#include <aeronet/websocket-client.hpp>

#include <chrono>
#include <cstddef>
#include <span>
#include <string_view>

// The state of the callbacks of a connection, given to connect().
struct Ticker {
  int nbMessages{0};
};

Ticker ticker;
WebSocketClient client;
const WebSocketConnectResult result = client.connect(
    "wss://ws.kraken.com/v2",
    WebSocketClientCallbacks{
        .onOpen =
            [](WebSocketConnection& cnx) {
              // The first messages of the connection, such as its subscriptions.
              cnx.sendText(R"({"method":"subscribe","params":{"channel":"ticker","symbol":["BTC/USD"]}})");
            },
        .onMessage =
            [](WebSocketConnection& cnx, std::span<const std::byte> payload, bool isBinary) {
              // A complete message, only valid during the call.
              ++static_cast<Ticker*>(cnx.userData())->nbMessages;
              const std::string_view text(reinterpret_cast<const char*>(payload.data()), payload.size());
              if (!isBinary && text.contains("\"error\"")) {
                cnx.close(websocket::CloseCode::Normal, "bye");
              }
            },
        .onPong = {},
        .onClose =
            [](WebSocketConnection& /*cnx*/, websocket::CloseCode code, std::string_view reason) {
              // Called once, when the connection ends: the code and reason of the server, or why it ended.
            },
    },
    &ticker);
if (result) {
  while (client.poll(std::chrono::seconds{1}) != 0) {
    // Until every connection ended.
  }
}
```

## Command-line client example

The [websocket-client example](../../examples/websocket-client.cpp) (binary `aeronet-websocket-client`) is a small command-line client in the spirit of `wscat`, built on the calls of this page. Once the connection is open, it sends the messages given after the URL, then prints each message of the server on its own line of the standard output, binary ones in hexadecimal. The status of the connection goes to the standard error, and the logs of aeronet are off unless `--verbose` is given, so that the messages can be piped to a tool like `jq`. The Kraken ticker above, reduced to its prices for 10 seconds:

```bash
./build/examples/aeronet-websocket-client --duration 10 wss://ws.kraken.com/v2 \
  '{"method":"subscribe","params":{"channel":"ticker","symbol":["BTC/USD","ETH/USD"]}}' |
  jq -c 'select(.channel == "ticker") | .data[] | {symbol, bid, ask, last}'
```

| Option | Effect |
| --- | --- |
| `-n`, `--max-messages <count>` | Close the connection after `<count>` received messages. The ones still in flight are not printed. |
| `-t`, `--duration <seconds>` | Close the connection after `<seconds>`. |
| `-H`, `--header 'Name: value'` | Add a header to the upgrade request, such as credentials or `Origin`. Repeatable. |
| `-p`, `--protocol <list>` | Offer subprotocols, comma-separated. The one selected by the server is printed. |
| `--no-compression` | Do not offer `permessage-deflate`. |
| `-v`, `--verbose` | Print the debug logs of aeronet, on the standard output. |
| `--` | End of the options: the next arguments are messages, even when they start with `-`. |

Ctrl+C starts the closing handshake. The program exits with `0` when the connection ended with a closing handshake of code `1000` or `1001`, and with `1` otherwise. When the handshake fails, it prints why, and the status and start of the body of a server refusing the upgrade. Against the [echo example](../../examples/websocket-echo.cpp):

```bash
./build/examples/aeronet-websocket-echo 8080 &
./build/examples/aeronet-websocket-client -n 2 ws://localhost:8080/ws hello world
```

## The opening handshake

`connect(url, callbacks)` accepts the `ws`, `wss`, `http` and `https` schemes. The handshake runs synchronously through the `HttpClient` of the client (`httpClient()`), so its [configuration](../reference/client-configuration.md) applies: connect and request timeouts, TLS settings, global headers and `User-Agent`, cookies, and the forward proxy, which is always crossed with a `CONNECT` tunnel, `ws://` URLs included. The same `HttpClient` can serve regular requests, such as fetching an authentication token before opening a connection.

The client adds `Sec-WebSocket-Key` (16 random bytes from the system generator), `Sec-WebSocket-Version: 13` and, when compression is enabled, the `permessage-deflate` offer. Over TLS, the handshake connection only offers `http/1.1` through ALPN, so that a server preferring HTTP/2 still answers over HTTP/1.1. Redirects are not followed, and the handshake is not retried.

The connection opens on a `101` response whose `Upgrade`, `Connection`, `Sec-WebSocket-Accept`, `Sec-WebSocket-Protocol` and `Sec-WebSocket-Extensions` headers are valid (RFC 6455 §4.1). A header split over several lines is checked whole, as the comma-separated list of its values (RFC 9110 §5.3): `Connection: keep-alive` followed by `Connection: Upgrade` is accepted, and a second `Sec-WebSocket-Protocol` or `Sec-WebSocket-Extensions` line fails the handshake when it holds a second subprotocol or an extension that was not offered. The offered subprotocols may be on several lines of the request as well. Otherwise `connect()` returns a `WebSocketConnectResult` without connection:

| Field | Content |
| --- | --- |
| `pConnection` | The open connection, `nullptr` when the handshake failed (`result` converts to `false`). |
| `response` | The response of the server: the `101` of an open connection, or the refusal (a `401`, a `429` with `Retry-After`...), or the invalid `101`. Empty after a transport failure. |
| `pErrorMessage` | Why the handshake failed (`nullptr` on success). |
| `httpError` | The `HttpClientErrc` of a transport failure (DNS, connect, TLS, timeout...), `noError` otherwise. |

Extra headers, such as credentials or the offered subprotocols, go into the upgrade request built by `makeUpgradeRequest()`. The subprotocol selected by the server is in the response:

```cpp
#include <aeronet/websocket-client.hpp>

#include <string_view>
#include <utility>

WebSocketClient client;
HttpRequest request = client.makeUpgradeRequest("wss://example.com/graphql");
request.headerAddLine(http::Authorization, "Bearer token");
request.headerAddLine(websocket::SecWebSocketProtocol, "graphql-transport-ws, graphql-ws");
const WebSocketConnectResult result = client.connect(std::move(request), WebSocketClientCallbacks{});
const std::string_view protocol = result.response.headerValueOrEmpty(websocket::SecWebSocketProtocol);
```

## Callbacks

`WebSocketClientCallbacks` holds the callbacks of a connection: function pointers, `nullptr` when not needed, to which lambdas without capture convert. Their state is the user data pointer given to `connect(url, callbacks, pUserData)`, not owned, that `WebSocketConnection::userData()` returns: the same callbacks can serve several connections, each with its own state. `onOpen` runs in `connect()`, before it returns, and the others in `poll()`, on the thread calling them. They may send messages and close connections, on any connection of the client, but must not call `connect()` or `poll()`.

A callback that throws ends its connection at once, without closing handshake nor `onClose`, and the exception propagates out of `connect()` or `poll()`: the received bytes already processed are unknown, so the connection cannot go on. The connection is destroyed before the exception leaves, and the other connections are served by the next `poll()`.

| Callback | Called |
| --- | --- |
| `onOpen(connection)` | Once, when the handshake succeeded. The messages it sends are the first ones of the connection. |
| `onMessage(connection, payload, isBinary)` | For each complete message: fragments reassembled, decompressed, valid UTF-8 for a text one. The payload is a view of the receive buffers, valid during the call. |
| `onPong(connection, payload)` | For each Pong frame, answering `sendPing()` or an automatic ping. |
| `onClose(connection, code, reason)` | Once, when the connection ends (see [Closing](#closing)). Nothing can be sent anymore. |

A `WebSocketConnection` belongs to its client: it stays valid until its `onClose` callback returns, one of its callbacks throws, or the client is destroyed.

## Sending messages

`sendText()`, `sendBinary()`, `sendPing()` and `close()` queue a frame, masked with a key from the system's secure generator (fetched 64 keys at a time). Out of the callbacks, the frame is written at once, as far as the socket takes it. From a callback, the frames are written together once `poll()` is done with the bytes it received. What the socket did not take is written when `poll()` finds it writable again: `bufferedAmount()` tells how many bytes wait, so that a fast sender can slow down. The send functions return `false` once the connection is closing, and in `onClose`. A failed write out of a callback is reported by the next `poll()`, never by the send call itself. A lost connection drops the frames it could not write: `bufferedAmount()` is `0` in its `onClose`.

A text message must be valid UTF-8 (asserted in debug builds only): the server fails the connection otherwise.

## Serving the connections

`poll(timeout)` waits for at most `timeout`, or until the next timeout of a connection, then reads what the servers sent, runs the callbacks, writes the queued frames, checks the timeouts, and removes the ended connections. It returns the number of connections not ended yet, so that `while (client.poll(...) != 0)` serves them until they all end. With no connection, it waits for `timeout`.

A connection reads at most 4 times per `poll()`, so that a fast feed does not starve the others; the rest of its bytes, including those already decrypted by TLS, are read by the next `poll()`, which then does not wait. Over TLS, a read may have to write first, such as the answer to a TLS 1.3 `KeyUpdate` while the socket takes no more bytes: the connection then waits for its socket to be writable too, reads again as soon as it is, and goes back to waiting for its readability only. Connections are looked up by a linear scan of their sockets, stored contiguously, on each event: the client is meant for a few tens of connections.

## Closing

`onClose` is called once per connection, with what ended it:

| End | `code` | `reason` |
| --- | --- | --- |
| Close frame of the server, answering ours or starting the closing handshake (answered automatically) | its code, `NoStatusReceived` without one | its reason |
| Failure detected by the client: masked frame, invalid frame, too large message, invalid UTF-8 | the code of the Close frame the client sent (`ProtocolError`, `MessageTooBig`, `InvalidPayloadData`...) | a description |
| Connection lost without Close frame: closed or reset by the server, idle timeout, no answer to our Close frame within `closeTimeout` | `AbnormalClosure` | a description |

After a closing handshake, the client waits for the server to close the TCP connection (RFC 6455 §7.1.1), for `closeTimeout` at most, then closes it. After a failure, it closes the connection once its Close frame is written. Destroying the client closes its connections at once, without closing handshake nor callback.

## Idle connections

A connection on which nothing was received for `idleTimeout` (60 s by default) ends with `AbnormalClosure`. With `autoPing` (the default), a Ping frame is sent once the connection has been idle for half of `idleTimeout`: the Pong of a live server keeps the connection open. Some servers expect pings of their own protocol instead (Kraken: `{"method":"ping"}`), to send from the application.

## Compression

When zlib is enabled, the client offers `permessage-deflate` unless `WebSocketConfig::deflateConfig.enabled` is `false`. The offer always holds `client_max_window_bits`, and the other parameters follow `DeflateConfig`: `serverMaxWindowBits` and `clientMaxWindowBits` below 15 request smaller windows, and `serverNoContextTakeover` / `clientNoContextTakeover` request a reset of the context after each message. The response of the server is checked (RFC 7692 §7): a parameter that is unknown, repeated, or with an invalid value, or a server window larger than requested, fails the handshake. The client compresses with the smallest of its configured window and of the one the server allows. Messages shorter than `minCompressSize` are sent uncompressed, and so are the ones compression would not shrink.

## Configuration

`WebSocketClient(websocket::WebSocketConfig config, HttpClientConfig httpConfig)` configures the connections and the handshakes. An invalid configuration throws `std::invalid_argument`.

| `WebSocketConfig` field | Default | Effect |
| --- | --- | --- |
| `maxMessageSize` | 64 MiB | Largest message, after reassembly and decompression: a larger one fails the connection with `MessageTooBig`. |
| `maxFrameSize` | 16 MiB | Largest frame payload. |
| `closeTimeout` | 5 s | Wait for the Close frame of the server after ours, and for the closing of the TCP connection after the handshake. |
| `idleTimeout` | 60 s | See [Idle connections](#idle-connections). `0` disables it. |
| `autoPing` | `true` | See [Idle connections](#idle-connections). |
| `deflateConfig` | offered | See [Compression](#compression). |

`isServerSide` is ignored. The reads use `HttpClientConfig::minReadChunkBytes` as their smallest chunk, and fill the whole free capacity of the receive buffer, which grows with the frame being received.

```cpp
#include <aeronet/websocket-client.hpp>

#include <chrono>

websocket::WebSocketConfig config;
config.idleTimeout = std::chrono::seconds{5};  // the server sends a heartbeat every second
config.maxMessageSize = 1024UL * 1024UL;
HttpClientConfig httpConfig;
httpConfig.connectTimeout = std::chrono::seconds{3};
WebSocketClient client(config, httpConfig);
```

## Other protocol upgrades

`HttpClient::upgrade()` is the lower-level part of the handshake, usable for any protocol switched to by a `101` response, such as the `tcp` upgrade of some container APIs. `makeUpgradeRequest(url, protocol)` builds the `GET` request with its `Upgrade` and `Connection` headers. On `101`, the `HttpUpgradeResponse` holds the connection: its transport, its socket, and the bytes of the new protocol received with the response. See [HTTP client](../modules/http-client.md#protocol-upgrades).

## Performance

- An unfragmented, uncompressed message is delivered to `onMessage` straight from the receive buffer, without copy. The receive buffer, the reassembly buffer of fragmented messages, the output buffer and the masking keys are reused: a steady stream of messages does not allocate.
- Frames sent from a callback are written with the others of the same `poll()`, in one write per connection.
- Over TLS, `SIGPIPE` is blocked around the writes only (OpenSSL writes on the socket itself): a connection that only receives makes no system call for it.
- [websocket-client_bench.cpp](../../benchmarks/internal/websocket-client_bench.cpp) measures the messages received and sent per second against a loopback server thread.

## Limitations

- The handshake is synchronous: while `connect()` runs, the other connections of the client are not served.
- There is no automatic reconnection yet: after `onClose`, call `connect()` again from the loop calling `poll()`, with a backoff.
- Messages are sent and received from the thread calling `poll()`: there is no thread-safe send.
- WebSocket over HTTP/2 (RFC 8441) is not supported, nor are extensions other than `permessage-deflate`.
- Messages are delivered whole, up to `maxMessageSize`: there is no streaming of large messages.

## Tests

- Against the WebSocket endpoints of an aeronet server: messages both ways (also sent from `onOpen` and across connections), the schemes, refused upgrades, transport failures, subprotocols, pings both ways and automatic pings, bursts of small messages, 8 MiB messages, compression both ways, the closing handshake from either side, `wss://` with a server preferring `h2` (`WebSocketClientTls`); callbacks that throw (`Throwing*`); and against scripted servers: a frame received with the `101` response, data after a Close frame, connections closed or reset by the server, masked frames (also with a Close frame that cannot be written), idle and close timeouts, blocked writes, headers split over several lines, invalid handshake responses, TLS reads that have to write first (with a scripted transport), event loop failures (`WebSocketClientScripted`): [websocket-client_test.cpp](../../aeronet/client/test/websocket-client_test.cpp).
- Key generation and validation of the `101` response: [websocket-client-handshake_test.cpp](../../aeronet/websocket/test/websocket-client-handshake_test.cpp). The `permessage-deflate` offer and the parsing of its response (`BuildDeflateOffer_*`, `ParseDeflateResponse*`): [websocket-deflate_test.cpp](../../aeronet/websocket/test/websocket-deflate_test.cpp).
- The [command-line client example](#command-line-client-example) exchanges two messages with the echo example, then closes the connection with a closing handshake, in the `Examples and Docs` job of `.github/workflows/ci.yml`.
- `HttpClient::upgrade()`, including the bytes received with the `101` response, cookies and proxy tunnels (`*Upgrade*`): [http-client-core_test.cpp](../../aeronet/client/test/http-client-core_test.cpp). The `101` ending an upgrade request in the response parser (`SwitchingProtocolsEndsAnUpgradeRequest`): [http1-response-parser_test.cpp](../../aeronet/client/test/http1-response-parser_test.cpp).
