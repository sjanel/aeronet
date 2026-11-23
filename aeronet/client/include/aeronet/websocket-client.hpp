#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "aeronet/connection.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/upgraded-connection.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/websocket-config.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-handler.hpp"

namespace aeronet {

class WebSocketClient;
class WebSocketConnection;

// Callbacks of a WebSocket connection of a WebSocketClient: function pointers, null when not needed (a lambda without
// capture converts to one). Their state comes from the user data pointer given to WebSocketClient::connect(), returned
// by WebSocketConnection::userData(). onOpen runs in WebSocketClient::connect(), the others in WebSocketClient::poll(),
// on the thread calling them. They may send messages and close connections, on any connection of the client, but not
// open one nor poll: connect() and poll() throw std::logic_error when called from a callback run by poll() (reconnect
// from the loop calling poll()). A callback that throws ends its connection at once, without closing handshake nor
// onClose: the exception propagates out of connect() or poll(), and the connection is destroyed.
struct WebSocketClientCallbacks {
  // The opening handshake succeeded: called once, by connect(), before it returns. The messages sent here are the first
  // ones of the connection (subscriptions...).
  void (*onOpen)(WebSocketConnection& connection){nullptr};

  // A complete message: fragments reassembled, decompressed, valid UTF-8 for a text one. The payload is a view of the
  // receive buffers, only valid during the call (no copy).
  void (*onMessage)(WebSocketConnection& connection, std::span<const std::byte> payload, bool isBinary){nullptr};

  // A Pong frame from the server, like the answer to sendPing() or to an automatic ping (WebSocketConfig::autoPing).
  void (*onPong)(WebSocketConnection& connection, std::span<const std::byte> payload){nullptr};

  // The connection ended, called once (the reason is only valid during the call):
  //  - Close frame of the server, answering ours or starting the closing handshake: its code and reason,
  //  NoStatusReceived
  //    when it has none.
  //  - Failure detected by the client (frame of the server breaking the protocol, too large message, invalid UTF-8):
  //    the code of the Close frame it sent, with a description.
  //  - Connection lost without Close frame (closed or reset by the server, idle timeout, unanswered Close frame after
  //    closeTimeout): AbnormalClosure, with a description.
  // The WebSocketConnection is destroyed after this call. Nothing can be sent anymore: the send functions return false.
  void (*onClose)(WebSocketConnection& connection, websocket::CloseCode code, std::string_view reason){nullptr};
};

// A WebSocket connection opened by WebSocketClient::connect(), owned by its client. It stays valid until its onClose
// callback returns, one of its callbacks throws, or the client is destroyed.
class WebSocketConnection {
  struct PrivateTag {
    explicit PrivateTag() = default;
  };

 public:
  // Built by WebSocketClient::connect() only.
  WebSocketConnection(PrivateTag, WebSocketClient& client, UpgradedConnection&& upgraded,
                      const WebSocketClientCallbacks& callbacks, void* pUserData);

  // Referred to by the callbacks of its handler: neither copyable nor movable.
  WebSocketConnection(const WebSocketConnection&) = delete;
  WebSocketConnection(WebSocketConnection&&) = delete;
  WebSocketConnection& operator=(const WebSocketConnection&) = delete;
  WebSocketConnection& operator=(WebSocketConnection&&) = delete;

  ~WebSocketConnection() = default;

  // Send a text message, valid UTF-8 (asserted in debug builds only, the server fails the connection otherwise). Out of
  // the callbacks, it is written at once as far as the socket takes it, the rest when poll() finds the socket writable.
  // From a callback, the messages are written together when poll() is done with the received bytes. Returns false once
  // the connection is closing (onClose included).
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
  // the server answers (onClose gets its Close frame), or after closeTimeout. Returns false when already closing.
  bool close(websocket::CloseCode code = websocket::CloseCode::Normal, std::string_view reason = {}) {
    return _state == State::Open && queued(_handler.sendClose(code, reason));
  }

  // Bytes queued but not written to the socket yet, which did not take them: a fast sender may slow down when it grows.
  [[nodiscard]] std::size_t bufferedAmount() const noexcept { return _handler.pendingOutputSize(); }

  // Whether messages can still be sent: the connection is not closing.
  [[nodiscard]] bool isOpen() const noexcept { return _state == State::Open && !_handler.isClosing(); }

  // The client owning this connection.
  [[nodiscard]] WebSocketClient& client() noexcept { return *_pClient; }

  // The user data pointer given to WebSocketClient::connect(), for the callbacks.
  [[nodiscard]] void* userData() const noexcept { return _pUserData; }

 private:
  friend class WebSocketClient;
  friend class WebSocketConnectionTest;

  enum class State : uint8_t {
    Open,      // messages flow, a closing handshake may be in progress (see WebSocketHandler::isClosing())
    Draining,  // onClose called after a Close frame, nothing more is sent: waiting for the server to close the TCP
               // connection (or for the Close frame failing the connection to be written)
    Ended,     // destroyed at the end of the current poll()
  };

  // Write the frames just queued (when 'ok'), now or later, see sendText(). Returns 'ok'.
  bool queued(bool ok);

  // Write as much pending output as the transport takes. Returns false on a transport or event loop error.
  bool flush();

  // Watch the writability of the socket while the pending output or the read waits for it. Returns false if the event
  // loop failed.
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

  // Call onClose once, with what ended the connection, which then drains (after a Close frame) or ends. An ended
  // connection sends nothing more.
  void notifyClose(websocket::CloseCode code, std::string_view reason, State nextState);

  // The connection is lost (EOF, error, timeout): it ends, then onClose if not called yet.
  void lose(std::string_view reason) { notifyClose(websocket::CloseCode::AbnormalClosure, reason, State::Ended); }

  // Earliest time at which the timeouts of this connection must be checked.
  [[nodiscard]] SteadyClock::time_point nextDeadline() const noexcept;

  // Check the idle and close timeouts at 'now' (automatic ping, end of the connection).
  void checkTimeouts(SteadyClock::time_point now);

  websocket::WebSocketHandler _handler;
  Transport _transport;
  RawChars _in;  // received bytes not processed yet (an incomplete frame)
  WebSocketClientCallbacks _callbacks;
  void* _pUserData;
  WebSocketClient* _pClient;
  SteadyClock::time_point _lastActivity;   // last time bytes were received
  SteadyClock::time_point _drainDeadline;  // end of the Draining state
  Connection _cnx;
  EventBmp _interest;  // events the event loop of the client watches for this connection
  State _state{State::Open};
  bool _pendingInput{false};      // received bytes to process without waiting for the socket (TLS, read budget)
  bool _unprocessedInput{false};  // _in holds bytes received with the 101 response, not processed yet
  bool _writeFailed{false};       // a send out of poll() failed: the next poll() ends the connection
  bool _writeBlocked{false};      // the socket took no more bytes: the pending output waits for its writability
  bool _readWantsWrite{false};    // TLS has to write before it reads again: the read waits for the writability too
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
// Connections are looked up by a linear scan of their sockets (contiguous) on each event: the client is meant for a few
// tens of connections.
// Thread-safety: none, like HttpClient. Each poll() and each connect() runs on the calling thread.
class WebSocketClient {
 public:
  // A client with the default configurations.
  WebSocketClient();

  // A client whose connections use 'config' (limits, timeouts, automatic ping, permessage-deflate offer: its
  // isServerSide is ignored), and whose handshakes use an HttpClient with 'httpConfig'.
  // Throws std::invalid_argument on an invalid configuration.
  explicit WebSocketClient(websocket::WebSocketConfig config, HttpClientConfig httpConfig = {});

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
  // run in poll(). 'pUserData' is the state of the callbacks (WebSocketConnection::userData()), not owned.
  // Throws std::invalid_argument if the url is invalid, std::logic_error if called from a callback run by poll().
  WebSocketConnectResult connect(std::string_view url, const WebSocketClientCallbacks& callbacks,
                                 void* pUserData = nullptr) {
    return connect(makeUpgradeRequest(url), callbacks, pUserData);
  }

  // Open a WebSocket connection with 'upgradeRequest', made by makeUpgradeRequest(), like connect(url, ...). The client
  // adds Sec-WebSocket-Key, Sec-WebSocket-Version and the permessage-deflate offer.
  WebSocketConnectResult connect(HttpRequest&& upgradeRequest, const WebSocketClientCallbacks& callbacks,
                                 void* pUserData = nullptr);

  // Serve the connections: wait for at most 'timeout' (or until the next timeout of a connection, or without waiting
  // when received bytes are left to process), read what the servers sent and run the callbacks, write the queued
  // frames, check the idle and close timeouts (automatic pings), and remove the ended connections.
  // Returns the number of open connections (not ended): a loop serving the connections until they end checks it.
  // Throws std::logic_error if called from a callback run by poll().
  std::size_t poll(std::chrono::milliseconds timeout);

  // Number of open connections (not ended).
  [[nodiscard]] std::size_t nbConnections() const noexcept;

  // The HttpClient running the opening handshakes. It can serve regular requests too, its cookie jar is shared.
  [[nodiscard]] HttpClient& httpClient() noexcept { return _http; }

  // The configuration of the connections.
  [[nodiscard]] const websocket::WebSocketConfig& config() const noexcept { return _config; }

 private:
  friend class WebSocketConnection;

  // Sec-WebSocket-Key, Sec-WebSocket-Version and the permessage-deflate offer header lines, added by connect().
  static constexpr std::size_t kHandshakeHeadersSize = 45U + 27U + 156U;

  // The connection whose socket is 'fd', which must be one of them.
  [[nodiscard]] WebSocketConnection& find(NativeHandle fd) const noexcept;

  // Ensure the event loop watches 'interest' for the socket of 'cnx'. Returns false if the event loop failed.
  bool watch(WebSocketConnection& cnx, EventBmp interest);

  // Unregister the ended connections from the event loop, and destroy them.
  void removeEndedConnections();

  websocket::WebSocketConfig _config;
  HttpClient _http;
  EventLoop _loop;
  vector<std::unique_ptr<WebSocketConnection>> _connections;
  vector<NativeHandle> _fds;  // socket of each connection, at its index: find() scans them without dereferencing
  vector<std::string_view> _fragments;  // reused to write the pending output of a connection in one call
  uint32_t _readChunkBytes;             // HttpClientConfig::minReadChunkBytes
  int _pollTimeoutMs{-1};               // timeout of the poll policy of _loop
  bool _inPoll{false};
};

}  // namespace aeronet
