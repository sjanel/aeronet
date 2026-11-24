#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

#include "aeronet/connection.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/retry-config.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/upgraded-connection.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/websocket-client-handshake.hpp"
#include "aeronet/websocket-config.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-handler.hpp"

namespace aeronet {

class WebSocketClient;
class WebSocketConnection;

namespace internal {
class UpgradeHandshake;
struct HostResolution;
struct WebSocketClientShared;
}  // namespace internal

// Callbacks of a WebSocket connection of a WebSocketClient: function pointers, null when not needed (a lambda without
// capture converts to one). Their state comes from the user data pointer given to WebSocketClient::connect(), returned
// by WebSocketConnection::userData(). They run on the thread calling poll() (or receive()), except onOpen for
// connect(), which runs it before returning. They may send messages, close connections, on any connection of the
// client, and open connections with connectAsync(), but not call connect(), poll() nor receive(): those throw
// std::logic_error when called from a callback run by poll(). A callback that throws ends its connection at once,
// without closing handshake, onClose nor reconnection: the exception propagates out of connect() or poll(), and the
// connection is destroyed.
struct WebSocketClientCallbacks {
  // The opening handshake succeeded: called once per handshake, so again after each reconnection. The messages sent
  // here are the first ones of the connection (subscriptions...). WebSocketConnection::handshakeResponse() points to
  // the 101 response of the server.
  void (*onOpen)(WebSocketConnection& connection){nullptr};

  // A complete message: fragments reassembled, decompressed, valid UTF-8 for a text one. The payload is a view of the
  // receive buffers, only valid during the call (no copy). Without it, the messages are kept for
  // WebSocketConnection::receive().
  void (*onMessage)(WebSocketConnection& connection, std::span<const std::byte> payload, bool isBinary){nullptr};

  // A Pong frame from the server, like the answer to sendPing() or to an automatic ping (WebSocketConfig::autoPing).
  void (*onPong)(WebSocketConnection& connection, std::span<const std::byte> payload){nullptr};

  // The connection ended, or an opening handshake run by poll() failed (the reason is only valid during the call):
  //  - Close frame of the server, answering ours or starting the closing handshake: its code and reason,
  //    NoStatusReceived when it has none.
  //  - Failure detected by the client (frame of the server breaking the protocol, too large message, invalid UTF-8):
  //    the code of the Close frame it sent, with a description.
  //  - Connection lost without Close frame (closed or reset by the server, idle timeout, unanswered Close frame after
  //    closeTimeout), or failed handshake: AbnormalClosure, with a description. For a handshake,
  //    WebSocketConnection::handshakeResponse() and handshakeError() tell what failed.
  // Nothing can be sent anymore: the send functions return false. When WebSocketConnection::reconnecting() is true, the
  // client connects again later (close() cancels it), and calls onOpen once connected. Otherwise, this is the last
  // callback: the connection is destroyed after it (after receive() returned its end, without onMessage).
  void (*onClose)(WebSocketConnection& connection, websocket::CloseCode code, std::string_view reason){nullptr};
};

// Automatic reconnection of the connections of a WebSocketClient, and pacing of its connection attempts.
struct WebSocketReconnectConfig {
  // Throws std::invalid_argument if invalid (see RetryConfig::validate()), or if minConnectInterval is negative.
  void validate() const;

  // Whether the connections reconnect: retry.maxAttempts above 1.
  [[nodiscard]] bool enabled() const noexcept { return retry.maxAttempts > 1; }

  // The opening handshakes of a connection that the client makes in a row, the first one included: a connection that
  // ends without close() (lost, closed by the server, failed) connects again while fewer than maxAttempts handshakes
  // were made since the last stable connection, one that stayed open for maxDelay at least. The delay before each one
  // is delayFor() of the number of handshakes since then, minus one: baseDelay after the loss of a stable connection,
  // then growing by multiplier up to maxDelay, with jitter. A refused handshake is retried only when its status is in
  // retryStatuses (after its Retry-After delay when honorRetryAfter is set), and an invalid 101 response is not:
  // the connection then ends. retryIdempotentAfterSend is not used.
  // maxAttempts is 1 by default: no reconnection. std::numeric_limits<uint32_t>::max() reconnects forever.
  RetryConfig retry{
      .baseDelay = std::chrono::seconds{1},
      .maxDelay = std::chrono::seconds{60},
      .maxAttempts = 1,
      .multiplier = 2.0F,
      .jitter = 0.1F,
  };

  // Minimum delay between the starts of two connection attempts of the client to a host, those of all its connections,
  // first ones included: an attempt coming sooner waits (connect() sleeps), also when several are due together (polls
  // coming late). Some CDNs ban an IP address for minutes after too many attempts (Cloudflare: about 150 in 10
  // minutes, so 4 s keeps below). 0 (the default) lets them go at once.
  std::chrono::milliseconds minConnectInterval{0};
};

// Result of WebSocketConnection::receive().
struct WebSocketReceiveResult {
  enum class Status : uint8_t {
    Message,  // payload and isBinary
    Timeout,  // nothing received within the timeout: the connection goes on
    Closed,   // the connection ended, after its last message: closeCode and closeReason are those of onClose
  };

  // Whether a message was received.
  [[nodiscard]] explicit operator bool() const noexcept { return status == Status::Message; }

  // The payload as text.
  [[nodiscard]] std::string_view text() const noexcept {
    return {reinterpret_cast<const char*>(payload.data()), payload.size()};
  }

  // The message (Message), valid until the next receive() of the connection.
  std::span<const std::byte> payload;
  // Why the connection ended (Closed), valid until the next call of poll() or receive() of the client.
  std::string_view closeReason;
  websocket::CloseCode closeCode{websocket::CloseCode::Normal};
  Status status{Status::Timeout};
  bool isBinary{false};
};

// A WebSocket connection opened by WebSocketClient::connect() or connectAsync(), owned by its client. It stays valid
// until its last onClose callback returns (see WebSocketClientCallbacks::onClose), one of its callbacks throws, it is
// closed before being open, or the client is destroyed.
class WebSocketConnection {
  struct PrivateTag {
    explicit PrivateTag() = default;
  };

 public:
  // Built by WebSocketClient only.
  WebSocketConnection(PrivateTag, WebSocketClient& client, std::unique_ptr<internal::UpgradeHandshake> handshake,
                      const WebSocketClientCallbacks& callbacks, void* pUserData, uint64_t id);

  // Referred to by the callbacks of its handler: neither copyable nor movable.
  WebSocketConnection(const WebSocketConnection&) = delete;
  WebSocketConnection(WebSocketConnection&&) = delete;
  WebSocketConnection& operator=(const WebSocketConnection&) = delete;
  WebSocketConnection& operator=(WebSocketConnection&&) = delete;

  ~WebSocketConnection();

  // Send a text message, valid UTF-8 (asserted in debug builds only, the server fails the connection otherwise). Out of
  // the callbacks, it is written at once as far as the socket takes it, the rest when poll() finds the socket writable.
  // From a callback, the messages are written together when poll() is done with the received bytes. Returns false
  // unless the connection is open and not closing (during a handshake, onClose included).
  bool sendText(std::string_view text) { return _state == State::Open && queued(_handler.sendText(text)); }

  // Send a binary message, like sendText().
  bool sendBinary(std::span<const std::byte> data) {
    return _state == State::Open && queued(_handler.sendBinary(data));
  }

  // Send a Ping frame (payload truncated to 125 bytes), answered by a Pong (see WebSocketClientCallbacks::onPong).
  bool sendPing(std::span<const std::byte> payload = {}) {
    return _state == State::Open && queued(_handler.sendPing(payload));
  }

  // Start the closing handshake: send a Close frame (reason truncated to fit a control frame). The connection ends when
  // the server answers (onClose gets its Close frame), or after closeTimeout, without reconnection. Before the
  // connection is open (handshake in progress, or waiting to reconnect), and in an onClose followed by a reconnection,
  // end the connection instead, without callback: it is destroyed by the next poll() (or once receive() returned its
  // end). Returns false when it was already closing or ended.
  bool close(websocket::CloseCode code = websocket::CloseCode::Normal, std::string_view reason = {});

  // Wait for the next message, for 'timeout' at most, serving all the connections of the client meanwhile (see
  // WebSocketClient::poll()), for a connection without onMessage callback, which keeps its messages for this call. It
  // polls once at least: a zero timeout reads what the socket already holds. The message stays valid until the next
  // call. It reads its socket only once all its messages were received, so that a
  // server cannot make it buffer more than a read. Once the connection ended (without reconnection), returns Closed
  // after the last message: the connection is then destroyed by the next call of poll() or receive() of the client.
  // Throws std::logic_error for a connection with an onMessage callback, or if called from a callback run by poll().
  WebSocketReceiveResult receive(std::chrono::milliseconds timeout);

  // Bytes queued but not written to the socket yet, which did not take them: a fast sender may slow down when it grows.
  [[nodiscard]] std::size_t bufferedAmount() const noexcept { return _handler.pendingOutputSize(); }

  // Whether messages can be sent: the connection is open and not closing.
  [[nodiscard]] bool isOpen() const noexcept { return _state == State::Open && !_handler.isClosing(); }

  // Whether the client will connect again: in an onClose followed by a reconnection, and until it is open again.
  [[nodiscard]] bool reconnecting() const noexcept { return _reconnecting; }

  // The response of the server to the opening handshake, valid during the callback: the 101 in onOpen, and in an
  // onClose reporting a failed handshake, the refusal (a 401, a 429...) or the invalid 101. Null otherwise: after a
  // transport failure (see handshakeError()), and in the onClose ending a session.
  [[nodiscard]] const HttpResponse* handshakeResponse() const noexcept;

  // The transport failure (DNS, connect, TLS, timeout...) of the handshake reported by onClose, noError otherwise.
  [[nodiscard]] HttpClientErrc handshakeError() const noexcept;

  // Identifier of the connection, unique in its client: the posts of other threads target it (WebSocketClient::post*).
  [[nodiscard]] uint64_t id() const noexcept { return _id; }

  // The client owning this connection.
  [[nodiscard]] WebSocketClient& client() noexcept { return *_pClient; }

  // The user data pointer given to WebSocketClient::connect(), for the callbacks.
  [[nodiscard]] void* userData() const noexcept { return _pUserData; }

 private:
  friend class WebSocketClient;
  friend class WebSocketConnectionTest;

  enum class State : uint8_t {
    Waiting,     // waiting for _stateDeadline before its next handshake (backoff, minConnectInterval)
    Connecting,  // handshake in progress (its host being resolved, or on its socket)
    Open,        // messages flow, a closing handshake may be in progress (see WebSocketHandler::isClosing())
    Draining,    // onClose called after a Close frame, nothing more is sent: waiting for the server to close the TCP
                 // connection (or for the Close frame failing the connection to be written), until _stateDeadline
    Ended,       // the session is over: reconnects (_reconnecting) or is destroyed at the end of the current poll()
  };

  // The messages kept for receive(), by a connection without onMessage callback.
  struct ReceiveQueue {
    // A message of 'bytes'.
    struct Entry {
      std::size_t size;
      bool isBinary;
    };

    [[nodiscard]] bool empty() const noexcept { return next == entries.size(); }

    RawBytes bytes;         // the payloads, one after the other
    vector<Entry> entries;  // the messages
    RawChars closeReason;   // of the last end of the connection
    std::size_t offset{0};  // in 'bytes' of the next message to return
    uint32_t next{0};       // index in 'entries' of the next message to return
    websocket::CloseCode closeCode{websocket::CloseCode::Normal};
  };

  // Whether the connection is in a session, open or draining.
  [[nodiscard]] bool inSession() const noexcept { return _state == State::Open || _state == State::Draining; }

  // Set the callbacks of the protocol handler, forwarding to those of the connection.
  void setHandlerCallbacks();

  // Start a session on the connection switched to WebSocket by the handshake, whose response is 'validation' (the
  // negotiated compression): the protocol state is fresh.
  void startSession(UpgradedConnection&& upgraded, const websocket::UpgradeResponseValidation& validation);

  // Write the frames just queued (when 'ok'), now or later, see sendText(). Returns 'ok'.
  bool queued(bool ok);

  // Write as much pending output as the transport takes. Returns false on a transport or event loop error.
  bool flush();

  // The events of its socket that the session waits for: readability (unless paused, but for a TLS write waiting for
  // it), writability while the pending output or a TLS read waits for it.
  [[nodiscard]] EventBmp sessionInterest() const noexcept;

  // Watch sessionInterest() for the socket. Returns false if the event loop failed.
  bool updateInterest();

  // Record whether the read waits for the socket to be writable (TLS has to write first), watched accordingly.
  // Returns false if the event loop failed.
  bool setReadWantsWrite(bool readWantsWrite);

  // Write the pending output, unless the socket refused bytes and is not known to be 'writable' again: the connection
  // is lost if the write fails.
  void writePending(bool writable);

  // Read what the server sent and process it, at most a few reads, so that the other connections are served too.
  void read(SteadyClock::time_point now);

  // Process the received bytes: callbacks, answers (Pong, Close) queued.
  void processInput();

  // Keep a message for receive(), and stop reading the socket until it is received.
  void keep(std::span<const std::byte> payload, bool isBinary);

  // Messages wait for receive(): stop watching the readability of the socket, whose next bytes stay in it (or in TLS)
  // until they are received. The connection is lost if the event loop fails.
  void pauseReads();

  // receive() took all the kept messages: read the socket again.
  void resume(SteadyClock::time_point now);

  // Call onClose once per session, with what ended it, which then drains (after a Close frame) or ends. Decides
  // whether the client reconnects. An ended connection sends nothing more.
  void notifyClose(websocket::CloseCode code, std::string_view reason, State nextState);

  // The connection is lost (EOF, error, timeout): it ends, then onClose if not called yet.
  void lose(std::string_view reason) { notifyClose(websocket::CloseCode::AbnormalClosure, reason, State::Ended); }

  // Earliest time at which the timeouts of this connection must be checked.
  [[nodiscard]] SteadyClock::time_point nextDeadline(SteadyClock::time_point now) const noexcept;

  // Check the idle and close timeouts at 'now' (automatic ping, end of the connection).
  void checkTimeouts(SteadyClock::time_point now);

  // The time of the last activity, for the idle timeout: 'now' while paused, its socket not being read.
  [[nodiscard]] SteadyClock::time_point lastActivity(SteadyClock::time_point now) const noexcept {
    return _paused ? now : _lastActivity;
  }

  websocket::WebSocketHandler _handler;
  Transport _transport;
  RawChars _in;  // received bytes not processed yet (an incomplete frame)
  WebSocketClientCallbacks _callbacks;
  // The opening handshake: during one, and for the whole life of a connection that may reconnect (its request).
  std::unique_ptr<internal::UpgradeHandshake> _handshake;
  std::shared_ptr<internal::HostResolution> _resolution;  // host being resolved by a thread for the handshake
  std::unique_ptr<ReceiveQueue> _pReceiveQueue;           // without onMessage callback only
  void* _pUserData;
  WebSocketClient* _pClient;
  uint64_t _id;
  SteadyClock::time_point _lastActivity;   // last time bytes were received
  SteadyClock::time_point _openedAt;       // start of the session
  SteadyClock::time_point _stateDeadline;  // end of the Waiting or Draining state
  Connection _cnx;
  EventBmp _interest{0};         // events the event loop of the client watches for the socket, 0 when not registered
  uint32_t _index;               // in the connections of the client
  uint32_t _attempt{0};          // handshakes since the last stable connection
  websocket::WebSocketKey _key;  // of the handshake in progress
  State _state{State::Waiting};
  bool _pendingInput{false};      // received bytes to process without waiting for the socket (TLS, read budget)
  bool _unprocessedInput{false};  // _in holds bytes received with the 101 response, not processed yet
  bool _writeFailed{false};       // a send out of poll() failed: the next poll() ends the connection
  bool _writeBlocked{false};      // the socket took no more bytes: the pending output waits for its writability
  bool _writeWantsRead{false};    // TLS has to read before it writes again: the readability is watched, even paused
  bool _readWantsWrite{false};    // TLS has to write before it reads again: the read waits for the writability too
  bool _paused{false};            // messages wait for receive(): the socket is not read
  bool _reconnecting{false};      // the client connects again after the end of the session
  bool _closeRequested{false};    // close() was called: no reconnection
  bool _endReported{false};       // receive() returned the end of the connection
};

// Result of WebSocketClient::connect().
struct WebSocketConnectResult {
  // Whether the connection is open.
  [[nodiscard]] explicit operator bool() const noexcept { return pConnection != nullptr; }

  // The response of the server: the 101 response of an open connection (its headers, such as Sec-WebSocket-Protocol),
  // else its refusal (a 401, a 429 with Retry-After...), or an invalid 101 response. Empty after a transport failure.
  HttpResponse response;
  // The open connection, owned by the client (see WebSocketConnection), null if the handshake failed.
  WebSocketConnection* pConnection{nullptr};
  // Why the handshake failed, null if the connection is open.
  const char* pErrorMessage{nullptr};
  // The transport failure of the handshake (DNS, connect, TLS, timeout...), noError otherwise.
  HttpClientErrc httpError{HttpClientErrc::noError};
};

// Synchronous WebSocket client (RFC 6455, with permessage-deflate of RFC 7692 when zlib is enabled), serving any number
// of connections from the thread calling poll(). The opening handshakes go through an HttpClient, whose configuration
// applies: timeouts, TLS, forward proxy (always through a CONNECT tunnel), global headers, cookies. That HttpClient
// can serve regular requests too (httpClient()), such as fetching a token before opening a connection.
//
// Usage:
//   WebSocketClient client;
//   client.connect("wss://example.com/feed", {.onMessage = [](WebSocketConnection& cnx, auto payload, bool) { ... }},
//                  &feedState);  // the state of the callbacks, cnx.userData()
//   while (client.poll(std::chrono::seconds{1}) != 0) {
//   }
//
// connect() runs the opening handshake before returning, connectAsync() lets poll() run it, between the service of the
// other connections, as for the reconnections (see WebSocketReconnectConfig). Connections are looked up by a linear
// scan of their sockets (contiguous) on each event: the client is meant for a few tens of connections.
// Thread-safety: like HttpClient, a client runs on one thread at a time, except postText(), postBinary(), postClose()
// and wakeup(), which any thread may call while the client exists (and is not moved).
class WebSocketClient {
 public:
  // A client with the default configurations.
  WebSocketClient();

  // A client whose connections use 'config' (limits, timeouts, automatic ping, permessage-deflate offer: its
  // isServerSide is ignored), whose handshakes use an HttpClient with 'httpConfig', and which reconnects as
  // 'reconnectConfig' tells. Throws std::invalid_argument on an invalid configuration.
  explicit WebSocketClient(websocket::WebSocketConfig config, HttpClientConfig httpConfig = {},
                           WebSocketReconnectConfig reconnectConfig = {});

  WebSocketClient(const WebSocketClient&) = delete;
  WebSocketClient& operator=(const WebSocketClient&) = delete;

  // Moves the client with its connections, which then belong to the new one. Not allowed during poll().
  WebSocketClient(WebSocketClient&& rhs) noexcept;
  WebSocketClient& operator=(WebSocketClient&& rhs) noexcept;

  // Closes the connections at once, without closing handshake nor callback.
  ~WebSocketClient();

  // The upgrade request connect(url, ...) would send to 'url' (ws, wss, http or https scheme), to complete with headers
  // before connect(HttpRequest&&, ...): authentication, Origin, Sec-WebSocket-Protocol (the offered subprotocols,
  // comma-separated, the selected one is in the response). Throws std::invalid_argument if the url is invalid.
  [[nodiscard]] HttpRequest makeUpgradeRequest(std::string_view url, std::size_t additionalCapacity = 0) {
    return _http.makeUpgradeRequest(url, websocket::UpgradeValue, additionalCapacity + kHandshakeHeadersSize);
  }

  // Open a WebSocket connection to 'url' (ws, wss, http or https scheme): the opening handshake runs synchronously,
  // bounded by the connect and request timeouts of the HTTP configuration, then onOpen is called. The other callbacks
  // run in poll(). 'pUserData' is the state of the callbacks (WebSocketConnection::userData()), not owned. A failed
  // handshake is not retried, but the open connection reconnects (see WebSocketReconnectConfig).
  // Throws std::invalid_argument if the url is invalid, std::logic_error if called from a callback run by poll().
  WebSocketConnectResult connect(std::string_view url, const WebSocketClientCallbacks& callbacks,
                                 void* pUserData = nullptr) {
    return connect(makeUpgradeRequest(url), callbacks, pUserData);
  }

  // Open a WebSocket connection with 'upgradeRequest', made by makeUpgradeRequest(), like connect(url, ...). The client
  // adds Sec-WebSocket-Key, Sec-WebSocket-Version and the permessage-deflate offer. Also throws std::invalid_argument
  // if the request has a file body or trailers.
  WebSocketConnectResult connect(HttpRequest&& upgradeRequest, const WebSocketClientCallbacks& callbacks,
                                 void* pUserData = nullptr);

  // Open a WebSocket connection to 'url' without waiting: the next poll() starts its handshake, run by the following
  // ones between the service of the other connections, then calls onOpen, or onClose when it fails (retried like a
  // reconnection, see WebSocketReconnectConfig). Callable from the callbacks.
  // Throws std::invalid_argument if the url is invalid, HttpClientException for a wss url with an invalid TLS
  // configuration.
  WebSocketConnection& connectAsync(std::string_view url, const WebSocketClientCallbacks& callbacks,
                                    void* pUserData = nullptr) {
    return connectAsync(makeUpgradeRequest(url), callbacks, pUserData);
  }

  // Open a WebSocket connection with 'upgradeRequest', made by makeUpgradeRequest(), like connectAsync(url, ...). Also
  // throws std::invalid_argument if the request has a file body or trailers.
  WebSocketConnection& connectAsync(HttpRequest&& upgradeRequest, const WebSocketClientCallbacks& callbacks,
                                    void* pUserData = nullptr);

  // Serve the connections: wait for at most 'timeout' (or until the next timeout of a connection, or without waiting
  // when received bytes are left to process), read what the servers sent and run the callbacks, write the queued
  // frames (and those posted by other threads), run the opening handshakes, check the idle and close timeouts
  // (automatic pings), reconnect, and remove the ended connections.
  // Returns the number of connections not ended (open, or connecting): a loop serving the connections until they end
  // checks it. Throws std::logic_error if called from a callback run by poll().
  std::size_t poll(std::chrono::milliseconds timeout);

  // Number of connections not ended (open, or connecting).
  [[nodiscard]] std::size_t nbConnections() const noexcept;

  // The connection whose id() is 'id', null when it ended.
  [[nodiscard]] WebSocketConnection* findConnection(uint64_t id) const noexcept;

  // Thread-safe: queue a text message for the connection whose id() is 'id', sent by the thread calling poll(), which
  // this wakes up. The message is copied. It is dropped if the connection is not open when poll() takes it.
  void postText(uint64_t id, std::string_view text);

  // Thread-safe: queue a binary message, like postText().
  void postBinary(uint64_t id, std::span<const std::byte> data);

  // Thread-safe: close the connection whose id() is 'id' from the thread calling poll(), like
  // WebSocketConnection::close(), which this wakes up.
  void postClose(uint64_t id, websocket::CloseCode code = websocket::CloseCode::Normal, std::string_view reason = {});

  // Thread-safe and async-signal-safe: make the poll() in progress, or the next one, return without waiting.
  void wakeup() const noexcept;

  // The descriptor of the event loop of the client, readable while poll() has events to process (epoll on Linux, kqueue
  // on macOS; kInvalidHandle on Windows): an outer event loop watches it, and calls poll() with a zero timeout once it
  // is readable or nextTimeout() elapsed.
  [[nodiscard]] NativeHandle eventLoopHandle() const noexcept { return _loop.fd(); }

  // How long an outer event loop may wait for eventLoopHandle() before calling poll(): zero when received bytes wait to
  // be processed, until the next timeout of a connection (handshake, idle, close, reconnection) otherwise,
  // std::chrono::milliseconds::max() when none applies.
  [[nodiscard]] std::chrono::milliseconds nextTimeout() const noexcept;

  // The HttpClient running the opening handshakes. It can serve regular requests too, its cookie jar is shared.
  [[nodiscard]] HttpClient& httpClient() noexcept { return _http; }

  // The configuration of the connections.
  [[nodiscard]] const websocket::WebSocketConfig& config() const noexcept { return _config; }

  // The reconnection configuration.
  [[nodiscard]] const WebSocketReconnectConfig& reconnectConfig() const noexcept { return _reconnect; }

 private:
  friend class WebSocketConnection;
  friend class WebSocketConnectionTest;

  using State = WebSocketConnection::State;

  // Sec-WebSocket-Version and the permessage-deflate offer header lines, added by connect() (the Sec-WebSocket-Key line
  // of each handshake is only added on the wire).
  static constexpr std::size_t kHandshakeHeadersSize = 27U + 156U;

  // The last connection attempt started to a host (minConnectInterval).
  struct HostAttempt {
    using trivially_relocatable = std::true_type;

    RawChars32 host;
    SteadyClock::time_point time;
  };

  // A new connection of 'upgradeRequest' (with the handshake headers of the client), not started.
  WebSocketConnection& addConnection(HttpRequest&& upgradeRequest, const WebSocketClientCallbacks& callbacks,
                                     void* pUserData);

  // The earliest time, from 'earliest', at which a connection attempt to the host of 'cnx' may start:
  // minConnectInterval after the last attempt started to that host. Only the started attempts count: a reconnection
  // planned after a long backoff does not delay a new connection, and the attempts due together start one after the
  // other.
  [[nodiscard]] SteadyClock::time_point connectSlot(const WebSocketConnection& cnx,
                                                    SteadyClock::time_point earliest) const noexcept;

  // An attempt of 'cnx' starts now: record it for its host (minConnectInterval).
  void recordConnectAttempt(const WebSocketConnection& cnx);

  // Make 'cnx' wait for its next handshake for 'delay' at least.
  void scheduleHandshake(WebSocketConnection& cnx, std::chrono::milliseconds delay);

  // Start the next handshake of 'cnx'.
  void startHandshake(WebSocketConnection& cnx);

  // The handshake of 'cnx' made progress: watch its socket, or complete it.
  void handshakeStepped(WebSocketConnection& cnx);

  // The handshake of 'cnx' got its response: open the connection, or fail.
  void handshakeDone(WebSocketConnection& cnx);

  // The handshake of 'cnx' failed with 'reason': onClose, then reconnection after 'retryDelay', unless negative (no
  // retry).
  void handshakeFailed(WebSocketConnection& cnx, std::string_view reason, std::chrono::milliseconds retryDelay);

  // Stop the handshake of 'cnx' in progress, if any, closing its socket.
  void abortHandshake(WebSocketConnection& cnx);

  // The session of 'cnx' ended: close its socket, then reconnect or end.
  void sessionEnded(WebSocketConnection& cnx);

  // The backoff delay before the next handshake of 'cnx'.
  [[nodiscard]] std::chrono::milliseconds backoffDelay(const WebSocketConnection& cnx);

  // Collect the resolutions and the posts of other threads.
  void serveWakeup();

  // Whether a connection has bytes to read (or a failure to report) without waiting for its socket. Lowers 'deadline'
  // to the earliest deadline of the connections.
  [[nodiscard]] bool readsWithoutWaiting(SteadyClock::time_point now, SteadyClock::time_point& deadline) const noexcept;

  // The connection whose socket 'fd' is watched, null if none.
  [[nodiscard]] WebSocketConnection* find(NativeHandle fd) const noexcept;

  // Ensure the event loop watches 'interest' for 'fd', the socket of 'cnx' (0: not watched). The socket of a connection
  // must not be watched anymore when it changes. Returns false if the event loop failed.
  bool watch(WebSocketConnection& cnx, NativeHandle fd, EventBmp interest);

  // Stop watching the socket of 'cnx'.
  void unwatch(WebSocketConnection& cnx) noexcept;

  // Reconnect the ended connections, or unregister them from the event loop and destroy them (those receive() did not
  // report yet stay).
  void removeEndedConnections();

  // Destroy the connection at 'pos', closing its socket.
  void removeConnection(uint32_t pos);

  websocket::WebSocketConfig _config;
  WebSocketReconnectConfig _reconnect;
  HttpClient _http;
  EventLoop _loop;
  vector<std::unique_ptr<WebSocketConnection>> _connections;
  // The socket watched for each connection, at its index: find() scans them without dereferencing the connections.
  vector<NativeHandle> _fds;
  vector<std::string_view> _fragments;  // reused to write the pending output of a connection in one call
  vector<HostAttempt> _hostAttempts;    // the last attempt to each host, while within minConnectInterval
  // Shared with other threads: the wakeup of poll(), the frames they post, the host resolver (stable address).
  std::unique_ptr<internal::WebSocketClientShared> _shared;
  WebSocketConnection* _pServed{nullptr};  // the connection being served by poll(), ended if a callback throws
  uint64_t _nextId{1};
  uint32_t _readChunkBytes;  // HttpClientConfig::minReadChunkBytes
  int _pollTimeoutMs{-1};    // timeout of the poll policy of _loop
  bool _inPoll{false};
};

}  // namespace aeronet
