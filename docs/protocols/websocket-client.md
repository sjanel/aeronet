# WebSocket client

`WebSocketClient` opens WebSocket connections ([RFC 6455](https://www.rfc-editor.org/rfc/rfc6455)) to servers, such as market data feeds or browser automation endpoints, and serves any number of them from the thread calling its `poll()` method. Messages compressed with `permessage-deflate` ([RFC 7692](https://www.rfc-editor.org/rfc/rfc7692)) are supported. Connections open without holding the others, reconnect by themselves, deliver their messages to callbacks or to `receive()`, and accept messages from other threads; the client can also sit inside an event loop of the application.

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
| `-r`, `--reconnect` | Connect again when the connection fails or ends, with a backoff from 1 s to 60 s and attempts 4 s apart at least (see [Reconnection](#reconnection)). The messages are sent again by each new connection, like subscriptions. |
| `--no-compression` | Do not offer `permessage-deflate`. |
| `-v`, `--verbose` | Print the debug logs of aeronet, on the standard output. |
| `--` | End of the options: the next arguments are messages, even when they start with `-`. |

The example opens its connection with `connectAsync()`. Ctrl+C starts the closing handshake, or stops a handshake in progress or a reconnection. The program exits with `0` when the connection ended with a closing handshake of code `1000` or `1001`, and with `1` otherwise. When the handshake fails, it prints why, and the status and start of the body of a server refusing the upgrade. Against the [echo example](../../examples/websocket-echo.cpp):

```bash
./build/examples/aeronet-websocket-echo 8080 &
./build/examples/aeronet-websocket-client -n 2 ws://localhost:8080/ws hello world
```

## The opening handshake

`connect(url, callbacks)` accepts the `ws`, `wss`, `http` and `https` schemes, like `connectAsync()`. The handshake runs synchronously in `connect()` ([without blocking](#non-blocking-handshakes) with `connectAsync()`), through the `HttpClient` of the client (`httpClient()`), so its [configuration](../reference/client-configuration.md) applies: connect and request timeouts, TLS settings, global headers and `User-Agent`, cookies, and the forward proxy, which is always crossed with a `CONNECT` tunnel, `ws://` URLs included. The same `HttpClient` can serve regular requests, such as fetching an authentication token before opening a connection.

The client adds `Sec-WebSocket-Version: 13` and, when compression is enabled, the `permessage-deflate` offer, to the request, and a `Sec-WebSocket-Key` of 16 random bytes from the system generator to each handshake, on the wire only. Over TLS, the handshake connection only offers `http/1.1` through ALPN, so that a server preferring HTTP/2 still answers over HTTP/1.1. Redirects are not followed, and `connect()` does not retry a failed handshake: the [reconnection](#reconnection) applies to its connection once open. Each phase of the handshake is bounded like a request of the `HttpClient`: `connectTimeout` for the name resolution, the TCP connect (falling back to the next address of the host when one fails), the proxy tunnel and the TLS handshake each, then `requestTimeout` for the request and its response, from the end of the TLS handshake, with `inactivityTimeout` between their reads and writes. As for a request, an expired resolution or TCP connect fails with `connectFailed` (`ConnectNotCompletingFailsAtTheConnectTimeout` in [upgrade-handshake_test.cpp](../../aeronet/client/test/upgrade-handshake_test.cpp)), the other phases with `timeout`. A refusal sent before the server read all of the request is read while it is written.

The connection opens on a `101` response whose `Upgrade`, `Connection`, `Sec-WebSocket-Accept`, `Sec-WebSocket-Protocol` and `Sec-WebSocket-Extensions` headers are valid (RFC 6455 §4.1). A header split over several lines is checked whole, as the comma-separated list of its values (RFC 9110 §5.3): `Connection: keep-alive` followed by `Connection: Upgrade` is accepted, and a second `Sec-WebSocket-Protocol` or `Sec-WebSocket-Extensions` line fails the handshake when it holds a second subprotocol or an extension that was not offered. The offered subprotocols may be on several lines of the request as well. Otherwise `connect()` returns a `WebSocketConnectResult` without connection (`connectAsync()` reports it to `onClose`):

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

### Non-blocking handshakes

`connect()` blocks the calling thread for its handshake, so the other connections of the client are not served meanwhile. `connectAsync()` returns the connection at once instead: the next `poll()` starts its handshake, run by the following ones between the service of the other connections, then calls `onOpen`, or `onClose` when it fails. The host name is resolved by a few background threads of the client (`getaddrinfo` can block for seconds), except a numeric address. The reconnections use the same non-blocking handshake.

```cpp
#include <aeronet/websocket-client.hpp>

#include <chrono>
#include <span>
#include <string_view>

// The state of the callbacks of a connection.
struct Feed {
  int nbMessages{0};
  int lastRefusalStatus{0};
};

Feed bookFeed;
Feed tradeFeed;
const WebSocketClientCallbacks callbacks{
    .onOpen = [](WebSocketConnection& cnx) { cnx.sendText(R"({"method":"subscribe"})"); },
    .onMessage =
        [](WebSocketConnection& cnx, std::span<const std::byte> /*payload*/, bool /*isBinary*/) {
          ++static_cast<Feed*>(cnx.userData())->nbMessages;
        },
    .onPong = {},
    .onClose =
        [](WebSocketConnection& cnx, websocket::CloseCode /*code*/, std::string_view /*reason*/) {
          // A failed handshake: the refusal of the server (its status, headers and body), if any.
          if (const HttpResponse* pResponse = cnx.handshakeResponse()) {
            static_cast<Feed*>(cnx.userData())->lastRefusalStatus = pResponse->status();
          }
        },
};
WebSocketClient client;
client.connectAsync("wss://books.example.com/feed", callbacks, &bookFeed);
client.connectAsync("wss://trades.example.com/feed", callbacks, &tradeFeed);
while (client.poll(std::chrono::seconds{1}) != 0) {
  // The connections open, connecting, or waiting to reconnect.
}
```

A connection that is not open yet does not send: its send functions return `false`. `close()` stops its handshake at once, without callback, and `nbConnections()` counts it from the start.

## Callbacks

`WebSocketClientCallbacks` holds the callbacks of a connection: function pointers, `nullptr` when not needed, to which lambdas without capture convert. Their state is the user data pointer given to `connect(url, callbacks, pUserData)`, not owned, that `WebSocketConnection::userData()` returns: the same callbacks can serve several connections, each with its own state. `onOpen` runs in `connect()`, before it returns, and the other callbacks (`onOpen` too, for `connectAsync()` and the reconnections) in `poll()`, on the thread calling it. They may send messages, close connections, on any connection of the client, and open connections with `connectAsync()`, but not call `connect()`, `poll()` nor `receive()`: called from a callback run by `poll()`, these throw `std::logic_error`, which ends the connection of that callback like any exception (`ConnectAndPollFromACallbackThrow`, `ConnectAsyncFromACallback`).

A callback that throws ends its connection at once, without closing handshake, `onClose` nor reconnection, and the exception propagates out of `connect()` or `poll()`: the received bytes already processed are unknown, so the connection cannot go on. The connection is destroyed before the exception leaves, and the other connections are served by the next `poll()`, also the handshakes whose host was resolved with that of the throwing one (`ThrowingCallbackKeepsTheResolutionsServedWithIt`).

| Callback | Called |
| --- | --- |
| `onOpen(connection)` | Once per successful handshake: after each reconnection too. The messages it sends are the first ones of the connection, such as its subscriptions. `handshakeResponse()` points to the `101` response. |
| `onMessage(connection, payload, isBinary)` | For each complete message: fragments reassembled, decompressed, valid UTF-8 for a text one. The payload is a view of the receive buffers, valid during the call. Without it, the messages are kept for [receive()](#pulling-messages-with-receive). |
| `onPong(connection, payload)` | For each Pong frame, answering `sendPing()` or an automatic ping. |
| `onClose(connection, code, reason)` | When the connection ends, or when a handshake run by `poll()` fails (see [Closing](#closing)). Nothing can be sent anymore. `reconnecting()` tells whether the client connects again. |

A `WebSocketConnection` belongs to its client: it stays valid until its last `onClose` returns (the one without reconnection), one of its callbacks throws, it is closed before being open, or the client is destroyed. Without `onMessage`, it stays valid after its last `onClose` until `receive()` returned its end. Its `id()`, unique in the client, never changes, reconnections included.

## Sending messages

`sendText()`, `sendBinary()`, `sendPing()` and `close()` queue a frame, masked with a key from the system's secure generator (fetched 64 keys at a time). Out of the callbacks, the frame is written at once, as far as the socket takes it. From a callback, the frames are written together once `poll()` is done with the bytes it received. What the socket did not take is written when `poll()` finds it writable again: `bufferedAmount()` tells how many bytes wait, so that a fast sender can slow down. The send functions return `false` once the connection is closing, in `onClose`, and while it is not open (during a handshake, or waiting to reconnect): nothing is queued for a future connection. A failed write out of a callback is reported by the next `poll()`, never by the send call itself. A lost connection drops the frames it could not write: `bufferedAmount()` is `0` in its `onClose`.

A text message must be valid UTF-8 (asserted in debug builds only): the server fails the connection otherwise.

## Pulling messages with receive()

Request / response protocols, such as the Chrome DevTools Protocol, read better as a sequence of calls than as callbacks. A connection without `onMessage` keeps its messages for `receive(timeout)`, which returns the next one, serving all the connections of the client meanwhile (it calls `poll()` while it waits). The message stays valid until the next call:

```cpp
#include <aeronet/websocket-client.hpp>

#include <chrono>
#include <string_view>

WebSocketClient client;
const WebSocketConnectResult result =
    client.connect("ws://127.0.0.1:9222/devtools/browser/id", WebSocketClientCallbacks{});
if (result) {
  WebSocketConnection& cnx = *result.pConnection;
  cnx.sendText(R"({"id":1,"method":"Target.getTargets"})");
  for (;;) {
    const WebSocketReceiveResult received = cnx.receive(std::chrono::seconds{5});
    if (!received) {
      // Timeout (the connection goes on), or Closed: received.closeCode and received.closeReason, as onClose got them.
      break;
    }
    if (received.text().starts_with(R"({"id":1,)")) {
      break;  // the response, after the events received before it
    }
  }
}
```

The connection reads its socket only once all its messages were received: a server sending faster than the application receives is held back by TCP, instead of filling the memory of the client. Meanwhile its idle timeout does not count, nor are the Pong frames or the Close frame read. A TLS write that has to read first (a renegotiation of TLS 1.2) still gets the readability of the socket, without any message being read (`TlsWriteWaitingForReadabilityWhileTheReadsArePaused`). `receive()` polls once at least: with a zero timeout, it is a non-blocking probe, reading what the socket already holds (`ReceiveWithoutTimeoutReadsTheSocket`). Once the connection ended (without reconnection), `receive()` returns its last messages, then `Closed`; the connection is destroyed by the next `poll()`, which `receive()` calls when it waits. Across a reconnection, the messages of both connections follow each other. `receive()` throws `std::logic_error` for a connection with `onMessage`, and from a callback run by `poll()` (`ReceiveMisuseThrows`).

## Serving the connections

`poll(timeout)` waits for at most `timeout`, or until the next timeout of a connection, then reads what the servers sent, runs the callbacks, writes the queued frames (those posted by other threads too), runs the handshakes, checks the timeouts, starts the reconnections, and removes the ended connections. It returns the number of connections not ended yet, open, connecting or waiting to reconnect, so that `while (client.poll(...) != 0)` serves them until they all end. With no connection, it waits for `timeout`.

A connection reads at most 4 times per `poll()`, so that a fast feed does not starve the others; the rest of its bytes, including those already decrypted by TLS, are read by the next `poll()`, which then does not wait. Over TLS, a read may have to write first, such as the answer to a TLS 1.3 `KeyUpdate` while the socket takes no more bytes: the connection then waits for its socket to be writable too, reads again as soon as it is, and goes back to waiting for its readability only. Connections are looked up by a linear scan of their sockets, stored contiguously, on each event: the client is meant for a few tens of connections.

## Closing

`onClose` is called each time a connection ends, or a handshake run by `poll()` fails, with what ended it:

| End | `code` | `reason` |
| --- | --- | --- |
| Close frame of the server, answering ours or starting the closing handshake (answered automatically) | its code, `NoStatusReceived` without one | its reason |
| Failure detected by the client: masked frame, invalid frame, too large message, invalid UTF-8 | the code of the Close frame the client sent (`ProtocolError`, `MessageTooBig`, `InvalidPayloadData`...) | a description |
| Connection lost without Close frame: closed or reset by the server, idle timeout, no answer to our Close frame within `closeTimeout` | `AbnormalClosure` | a description |
| Handshake failed: transport failure (`handshakeError()` tells which), refusal or invalid `101` response (`handshakeResponse()` points to it) | `AbnormalClosure` | a description |

After a closing handshake, the client waits for the server to close the TCP connection (RFC 6455 §7.1.1), for `closeTimeout` at most, then closes it, and reconnects afterwards if it does. After a failure, it closes the connection once its Close frame is written. Destroying the client closes its connections at once, without closing handshake nor callback.

## Reconnection

With a `WebSocketReconnectConfig` whose `retry.maxAttempts` is above 1, a connection that ends without `close()` connects again, after a backoff delay: lost, closed by the server (whatever the code of its Close frame), failed by the client, or whose handshake failed. Its `onClose` sees `reconnecting()` true, the same `WebSocketConnection` stays valid, keeps its `id()` and user data, and its `onOpen` runs again once connected, to replay its subscriptions. `close()` cancels the reconnection: in that `onClose`, or later while the connection waits or connects again.

```cpp
#include <aeronet/websocket-client.hpp>

#include <chrono>
#include <cstdint>
#include <limits>

WebSocketReconnectConfig reconnect;
reconnect.retry.maxAttempts = std::numeric_limits<uint32_t>::max();  // forever
reconnect.retry.baseDelay = std::chrono::seconds{1};
reconnect.retry.maxDelay = std::chrono::seconds{60};
reconnect.retry.jitter = 0.1F;
// Cloudflare bans an IP address for minutes after about 150 connection attempts in 10 minutes.
reconnect.minConnectInterval = std::chrono::seconds{4};
WebSocketClient client(websocket::WebSocketConfig{}, HttpClientConfig{}, reconnect);
```

The `RetryConfig` fields of `HttpClient` drive it:

- `maxAttempts` bounds the handshakes made in a row since the last stable connection, the first one included: `1` (the default) never reconnects, `std::numeric_limits<uint32_t>::max()` reconnects forever. A connection is stable once it stayed open for `maxDelay`: its loss starts a new series.
- The delay before a handshake is `baseDelay` after the loss of a stable connection, then grows by `multiplier` with each handshake of the series, up to `maxDelay`, scaled by a random factor within `jitter` (`ReconnectionGivesUpAfterMaxAttempts`, `StableConnectionsRestartTheAttempts`).
- A refused handshake is retried only when its status is in `retryStatuses` (`429` and `503` by default), after the delay of its `Retry-After` header when `honorRetryAfter` is set (`RefusalsInRetryStatusesAreRetried`). Another refusal (a `401`...) or an invalid `101` response ends the connection (`RefusalsNotInRetryStatusesAreNotRetried`).
- `retryIdempotentAfterSend` is not used.

`minConnectInterval` spaces out the starts of the connection attempts of the client to a host, those of all its connections and their first ones included: an attempt coming sooner waits for its turn, so that many connections to a host, or a server closing them repeatedly, do not get the IP address of the client banned (`MinConnectIntervalSpacesTheAttemptsToAHost`). The attempts due together, such as when `poll()` is called late, start one after the other (`MinConnectIntervalSpacesTheAttemptsDueTogether`). Only the attempts started count: a new connection does not wait behind a reconnection planned later, after a long backoff (`MinConnectIntervalIgnoresAPlannedReconnection`). `connect()` sleeps for it, `connectAsync()` and the reconnections wait in `poll()`.

`connect()` returns the failure of its handshake without retrying it, while `connectAsync()` retries its first handshake like a reconnection.

## Other threads

The client runs on the thread calling `poll()`, but other threads can send messages through it: `postText(id, text)`, `postBinary(id, data)` and `postClose(id, code, reason)` copy the message into a queue of the client, then wake the `poll()` in progress (or the next one) up, which sends it as from a callback. The target is the `id()` of a connection: a message for a connection that ended, or that is not open when `poll()` takes it, is dropped. `wakeup()` only makes `poll()` return, such as for a loop to check a stop request; it is async-signal-safe. These four functions may be called from any thread while the client exists and is not moved.

```cpp
#include <aeronet/websocket-client.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <thread>

WebSocketClient client;
const WebSocketConnectResult result = client.connect(
    "wss://example.com/orders",
    WebSocketClientCallbacks{.onOpen = {},
                             .onMessage = [](WebSocketConnection& /*cnx*/, std::span<const std::byte> /*payload*/,
                                             bool /*isBinary*/) {},
                             .onPong = {},
                             .onClose = {}});
if (result) {
  std::atomic<bool> stop{false};
  std::jthread strategy([&client, &stop, id = result.pConnection->id()] {
    client.postText(id, R"({"order":"buy"})");  // sent by the thread calling poll()
    stop.store(true);
    client.wakeup();
  });
  while (!stop.load() && client.poll(std::chrono::seconds{10}) != 0) {
  }
}
```

The queue is shared by all the connections of the client, under a mutex: the thread calling `poll()` takes the whole queue at once, so a burst of posts costs it one lock and one wakeup.

## Outer event loops

A client can be one source of events of an application event loop: `eventLoopHandle()` is the descriptor of the event loop of the client (epoll on Linux, kqueue on macOS), readable while `poll()` has events to process, and `nextTimeout()` tells how long the outer loop may wait before calling `poll()` anyway, for the timeouts of the connections (handshakes, idle, close, reconnections): zero when received bytes wait to be processed, `std::chrono::milliseconds::max()` when no timeout applies. The outer loop calls `poll()` with a zero timeout once the descriptor is readable or the time elapsed:

```cpp
#include <aeronet/websocket-client.hpp>
#include <sys/epoll.h>

#include <algorithm>
#include <chrono>

WebSocketClient client;
const int epollFd = ::epoll_create1(0);
epoll_event watched{};
watched.events = EPOLLIN;
watched.data.fd = client.eventLoopHandle();
::epoll_ctl(epollFd, EPOLL_CTL_ADD, client.eventLoopHandle(), &watched);
for (int iteration = 0; iteration < 100; ++iteration) {
  const auto timeout = std::min(client.nextTimeout(), std::chrono::milliseconds{1000});
  epoll_event ready[16];
  ::epoll_wait(epollFd, ready, 16, static_cast<int>(timeout.count()));
  // ... the other descriptors of the application
  client.poll(std::chrono::milliseconds{0});
}
```

`eventLoopHandle()` is `kInvalidHandle` on Windows, whose `WSAPoll` has no such descriptor (`OuterEventLoopWatchesTheClient`).

## Idle connections

A connection on which nothing was received for `idleTimeout` (60 s by default) ends with `AbnormalClosure`. With `autoPing` (the default), a Ping frame is sent once the connection has been idle for half of `idleTimeout`: the Pong of a live server keeps the connection open. Some servers expect pings of their own protocol instead (Kraken: `{"method":"ping"}`), to send from the application.

## Compression

When zlib is enabled, the client offers `permessage-deflate` unless `WebSocketConfig::deflateConfig.enabled` is `false`. The offer always holds `client_max_window_bits`, and the other parameters follow `DeflateConfig`: `serverMaxWindowBits` and `clientMaxWindowBits` below 15 request smaller windows, and `serverNoContextTakeover` / `clientNoContextTakeover` request a reset of the context after each message. The response of the server is checked (RFC 7692 §7): a parameter that is unknown, repeated, or with an invalid value, or a server window larger than requested, fails the handshake. The client compresses with the smallest of its configured window and of the one the server allows. Messages shorter than `minCompressSize` are sent uncompressed, and so are the ones compression would not shrink.

## Configuration

`WebSocketClient(websocket::WebSocketConfig config, HttpClientConfig httpConfig, WebSocketReconnectConfig reconnectConfig)` configures the connections, the handshakes and the reconnections. An invalid configuration throws `std::invalid_argument`.

| `WebSocketConfig` field | Default | Effect |
| --- | --- | --- |
| `maxMessageSize` | 64 MiB | Largest message, after reassembly and decompression: a larger one fails the connection with `MessageTooBig`. |
| `maxFrameSize` | 16 MiB | Largest frame payload. |
| `closeTimeout` | 5 s | Wait for the Close frame of the server after ours, and for the closing of the TCP connection after the handshake. Not negative, `std::chrono::milliseconds::max()` waits for the server forever (`DrainingWithoutCloseTimeoutWaitsForTheServer`). |
| `idleTimeout` | 60 s | See [Idle connections](#idle-connections). Positive, `std::chrono::milliseconds::max()` disables it. |
| `autoPing` | `true` | See [Idle connections](#idle-connections). |
| `deflateConfig` | offered | See [Compression](#compression). |

`isServerSide` is ignored. The reads use `HttpClientConfig::minReadChunkBytes` as their smallest chunk, and fill the whole free capacity of the receive buffer, which grows with the frame being received.

| `WebSocketReconnectConfig` field | Default | Effect |
| --- | --- | --- |
| `retry.maxAttempts` | 1 | Handshakes in a row since the last stable connection, see [Reconnection](#reconnection): `1` never reconnects, `std::numeric_limits<uint32_t>::max()` reconnects forever. `enabled()` tells whether it is above 1. |
| `retry.baseDelay` | 1 s | Delay before the first reconnection after the loss of a stable connection. Positive. |
| `retry.maxDelay` | 60 s | Largest delay, and the time after which an open connection is stable. Not below `baseDelay`. |
| `retry.multiplier` | 2 | Growth of the delay at each handshake of a series. At least 1. |
| `retry.jitter` | 0.1 | Random scaling of the delay, within ±10%, so that clients do not reconnect in lockstep. Between 0 and 1. |
| `retry.retryStatuses` | `429`, `503` | Refusals of the handshake that are retried. |
| `retry.honorRetryAfter` | `true` | A retried refusal waits for its delta-seconds `Retry-After`, capped at `maxDelay`. |
| `minConnectInterval` | 0 | Minimum delay between the starts of two connection attempts of the client to a host. Not negative. |

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
- A message kept for `receive()` is copied once, into a buffer of its connection reused from message to message.
- A message posted by another thread is copied once into the queue of the client, whose buffers are swapped with those of the thread calling `poll()` (no allocation once they reached their size); only the first post of a batch makes the system call waking `poll()` up.
- The opening handshakes do not allocate per phase: their request (with its proxy `CONNECT` line) is built once in one buffer, and their receive buffer becomes the one of the connection.
- [websocket-client_bench.cpp](../../benchmarks/internal/websocket-client_bench.cpp) measures the messages received (by `onMessage` or `receive()`) and sent (from the thread calling `poll()` or posted) per second against a loopback server thread.

## Limitations

- `connect()` is synchronous: while it runs, the other connections of the client are not served (`connectAsync()` is not).
- The reconnection policy is the one of the client: a connection that must not reconnect, such as a browser automation session, goes to another client, or cancels its reconnection with `close()` in `onClose`.
- The addresses of a host are tried one after the other, the next one after a refusal, not raced (no Happy Eyeballs, RFC 8305): a first address that does not answer at all fails the handshake after `connectTimeout`.
- Only the posts of other threads are thread-safe: the callbacks, `receive()` and the other functions run on the thread calling `poll()`.
- WebSocket over HTTP/2 (RFC 8441) is not supported, nor are extensions other than `permessage-deflate`.
- Messages are delivered whole, up to `maxMessageSize`: there is no streaming of large messages.

## Tests

- Against the WebSocket endpoints of an aeronet server: messages both ways (also sent from `onOpen` and across connections), the schemes, refused upgrades, transport failures, subprotocols, pings both ways and automatic pings, bursts of small messages, 8 MiB messages, compression both ways (also negotiated by `connectAsync()`), the closing handshake from either side, `wss://` with a server preferring `h2`, also with `connectAsync()` (`WebSocketClientTls`); callbacks that throw (`Throwing*`) or call `connect()` / `poll()` (`ConnectAndPollFromACallbackThrow`), upgrade requests with trailers; non-blocking handshakes (`ConnectAsync*`, also with a host name resolved by a thread, opened from a callback, and failing), a callback stopping another handshake (`CallbackStoppingAnotherHandshake`), a reconnection after a Close frame of the server (`ServerCloseFrameIsFollowedByAReconnection`), `minConnectInterval`, `receive()` (`Receive*`), posts from other threads, `wakeup()` (`WebSocketClientThreads`) and an outer event loop; and against scripted servers: a frame received with the `101` response, data after a Close frame, connections closed or reset by the server, masked frames (also with a Close frame that cannot be written), a write failing in the last pass of `poll()` whose `onClose` opens connections, idle and close timeouts, blocked writes, headers split over several lines, invalid handshake responses, TLS reads that have to write first and TLS writes that have to read first while the reads are paused (with scripted transports), event loop failures (`EventLoopFailure*`, also when a kept message pauses the reads, Linux only), handshakes stopped by `close()` or timing out (also during the resolution of their host), resolutions failing or ending after their connection, or served with one whose callback throws, reconnections after a lost connection or retried refusals, their limits, their cancellation, stable connections, and `receive()` around the end of a connection or a reconnection, or without timeout (`WebSocketClientScripted`): [websocket-client_test.cpp](../../aeronet/client/test/websocket-client_test.cpp).
- Key generation and validation of the `101` response: [websocket-client-handshake_test.cpp](../../aeronet/websocket/test/websocket-client-handshake_test.cpp). The `permessage-deflate` offer and the parsing of its response (`BuildDeflateOffer_*`, `ParseDeflateResponse*`): [websocket-deflate_test.cpp](../../aeronet/websocket/test/websocket-deflate_test.cpp).
- The [command-line client example](#command-line-client-example) exchanges two messages with the echo example, then closes the connection with a closing handshake, in the `Examples and Docs` job of `.github/workflows/ci.yml`.
- `HttpClient::upgrade()`, including the bytes received with the `101` response, cookies and proxy tunnels (`*Upgrade*`): [http-client-core_test.cpp](../../aeronet/client/test/http-client-core_test.cpp). The handshake driven step by step by an event loop, the fallback to the next address of a host, the event loop of the client after the connect to the last address failed, requests with a file body or trailers, proxies refusing or not answering, timeouts of each phase, TLS failures, malformed or too large responses, responses received in several reads, blocked and failed writes (to the server and to the proxy), answers before the end of the request (`AnswerBeforeTheEndOfTheRequest`), connects completing at once: [upgrade-handshake_test.cpp](../../aeronet/client/test/upgrade-handshake_test.cpp). A TLS handshake slower than `requestTimeout` (`SlowTlsHandshakeOfAnUpgradeLeavesTheRequestTimeoutToTheExchange`): [http-client-tls-e2e_test.cpp](../../aeronet/client/test/http-client-tls-e2e_test.cpp). The background resolution of host names: [host-resolver_test.cpp](../../aeronet/sys/test/host-resolver_test.cpp). The `101` ending an upgrade request in the response parser (`SwitchingProtocolsEndsAnUpgradeRequest`): [http1-response-parser_test.cpp](../../aeronet/client/test/http1-response-parser_test.cpp).
