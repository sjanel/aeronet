#include "aeronet/websocket-client.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

#include "aeronet/adaptive-poll-timeout.hpp"
#include "aeronet/chrono-lite.hpp"
#include "aeronet/connection.hpp"
#include "aeronet/event-fd.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/host-resolver.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-view.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/log.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/retry-config.hpp"
#include "aeronet/static-string-view-helpers.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/upgraded-connection.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/websocket-client-handshake.hpp"
#include "aeronet/websocket-config.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-handler.hpp"
#include "upgrade-handshake.hpp"

#ifdef AERONET_LINUX
#include "aeronet/sigpipe-blocker.hpp"
#endif

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/websocket-deflate.hpp"
#endif

namespace aeronet {

namespace {

// Makes room in 'vec' for one more element, growing it geometrically: the next emplace_back() cannot fail.
template <class Vector>
void ReserveOneMore(Vector& vec) {
  if (vec.size() == vec.capacity()) {
    vec.reserve(std::max<typename Vector::size_type>(2U * vec.size(), 4U));
  }
}

}  // namespace

namespace internal {

// What a WebSocketClient shares with other threads, at an address that does not change when the client moves.
struct WebSocketClientShared {
  // A frame posted by another thread.
  struct Posted {
    enum class Kind : uint8_t { Text, Binary, Close };

    uint64_t id;       // of the connection
    std::size_t size;  // of its payload in the posted bytes (the reason of a Close frame)
    websocket::CloseCode closeCode;
    Kind kind;
  };

  // Queue a frame for the connection 'id', and wake poll() up if it is the first one.
  void post(uint64_t id, Posted::Kind kind, std::span<const std::byte> payload, websocket::CloseCode closeCode) {
    bool first;
    {
      const std::scoped_lock lock(mutex);
      first = posted.empty();
      // Room for the frame first: its payload is not appended without it, which would shift the next ones.
      ReserveOneMore(posted);
      postedBytes.append(payload);
      posted.emplace_back(id, payload.size(), closeCode, kind);
    }
    if (first) {
      wakeup.send();
    }
  }

  EventFd wakeup;                 // signaled by the threads posting frames and resolving hosts
  HostResolver resolver{wakeup};  // the host names of the handshakes, resolved by threads
  std::mutex mutex;
  RawBytes postedBytes;   // payloads of 'posted', under 'mutex'
  vector<Posted> posted;  // under 'mutex'
  // Taken from the posted ones by poll(), swapped to keep their capacity.
  RawBytes takenBytes;
  vector<Posted> taken;
  vector<std::shared_ptr<HostResolution>> resolved;  // reused by poll()
};

}  // namespace internal

namespace {

// Reads of a connection per poll(): the other connections are served in between.
constexpr uint32_t kMaxReadsPerPoll = 4;

// The events watched for every open connection: its socket is readable, or closed by the server.
constexpr EventBmp kReadEvents = EventIn | EventRdHup;

// Event slots of the event loop of a client: it serves a few connections.
constexpr uint32_t kNbInitialEvents = 16;

// The retry delay of a failed handshake that is not retried.
constexpr std::chrono::milliseconds kNoRetry{-1};

// The reason of a handshake refused by the server.
constexpr const char* kNotSwitched = "the server did not switch to the WebSocket protocol";

// The Sec-WebSocket-Key line of a handshake, only added on the wire.
constexpr std::string_view kKeyLinePrefix = JoinStringView_v<http::CRLF, websocket::SecWebSocketKey, http::HeaderSep>;
using KeyLine = std::array<char, kKeyLinePrefix.size() + std::tuple_size_v<websocket::WebSocketKey>>;

// The Sec-WebSocket-Key line of 'key'.
KeyLine MakeKeyLine(const websocket::WebSocketKey& key) {
  KeyLine keyLine;
  static_cast<void>(Append(std::string_view(key.data(), key.size()), AppendFixed<kKeyLinePrefix>(keyLine.data())));
  return keyLine;
}

static_assert(http::HeaderSize(websocket::SecWebSocketVersion.size(), websocket::kWebSocketVersion.size()) +
                  http::HeaderSize(websocket::SecWebSocketExtensions.size(), 128U) ==
              27U + 156U);

websocket::WebSocketConfig ClientSideConfig(websocket::WebSocketConfig config) {
  config.isServerSide = false;
  config.validate();
  return config;
}

WebSocketReconnectConfig ValidatedReconnectConfig(WebSocketReconnectConfig config) {
  config.validate();
  return config;
}

// Thrown by connect(), poll() and receive() called from a callback run by poll().
[[noreturn]] void ThrowCalledFromCallback() {
  throw std::logic_error(
      "WebSocketClient::connect(), poll() and receive() cannot be called from a callback run by poll()");
}

// The poll timeout to wait from 'now' until 'deadline', rounded up so that the deadline has passed when it ends.
int PollTimeoutMs(SteadyClock::time_point now, SteadyClock::time_point deadline) {
  if (deadline <= now) {
    return 0;
  }
  const auto timeoutMs = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
  return static_cast<int>(std::min<decltype(timeoutMs)>(timeoutMs, std::numeric_limits<int>::max()));
}

// The value of the 'key' header of 'headers' as a single comma-separated list (RFC 9110 §5.3), empty when absent: the
// value of its line, or the values of its lines joined in 'joined' when it has several.
std::string_view CombinedHeaderValue(HeadersView headers, LowerAsciiKey key, RawChars& joined) {
  static constexpr std::string_view kSeparator = ", ";
  std::string_view value;
  uint32_t nbLines = 0;
  for (const http::HeaderView header : headers) {
    if (header.name != key.get()) {
      continue;
    }
    if (++nbLines == 1) {
      value = header.value;
      continue;
    }
    if (nbLines == 2) {
      joined.append(value);
    }
    joined.append(kSeparator);
    joined.append(header.value);
  }
  return nbLines < 2 ? value : std::string_view(joined);
}

// Check the 101 response of 'handshake' (RFC 6455 §4.1), which sent 'key'. Each header is validated with all its lines
// (a second Sec-WebSocket-Protocol or Sec-WebSocket-Extensions line of the response fails the connection), copied only
// when it has several of them.
websocket::UpgradeResponseValidation ValidateHandshake(internal::UpgradeHandshake& handshake,
                                                       const websocket::WebSocketKey& key,
                                                       const websocket::WebSocketConfig& config) {
  const HttpResponse& response = handshake.response();
  RawChars upgradeLines;
  RawChars connectionLines;
  RawChars acceptLines;
  RawChars protocolLines;
  RawChars extensionsLines;
  RawChars offeredProtocolsLines;
  const websocket::UpgradeResponseValidation validation = websocket::ValidateUpgradeResponse(
      {
          .upgrade = CombinedHeaderValue(response.headers(), http::Upgrade, upgradeLines),
          .connection = CombinedHeaderValue(response.headers(), http::Connection, connectionLines),
          .accept = CombinedHeaderValue(response.headers(), websocket::SecWebSocketAccept, acceptLines),
          .protocol = CombinedHeaderValue(response.headers(), websocket::SecWebSocketProtocol, protocolLines),
          .extensions = CombinedHeaderValue(response.headers(), websocket::SecWebSocketExtensions, extensionsLines),
      },
      std::string_view(key.data(), key.size()),
      // The offered subprotocols are those of the request, as sent.
      CombinedHeaderValue(handshake.request().headers(), websocket::SecWebSocketProtocol, offeredProtocolsLines),
      config);
  if (!validation.valid()) {
    log::error("WebSocket client: invalid opening handshake response: {}", validation.pErrorMessage);
  }
  return validation;
}

}  // namespace

void WebSocketReconnectConfig::validate() const {
  retry.validate();
  if (minConnectInterval < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("minConnectInterval must not be negative");
  }
}

WebSocketConnection::WebSocketConnection(PrivateTag /*tag*/, WebSocketClient& client,
                                         std::unique_ptr<internal::UpgradeHandshake> handshake,
                                         const WebSocketClientCallbacks& callbacks, void* pUserData, uint64_t id)
    : _handler(client._config),
      _callbacks(callbacks),
      _handshake(std::move(handshake)),
      _pReceiveQueue(callbacks.onMessage == nullptr ? std::make_unique<ReceiveQueue>() : nullptr),
      _pUserData(pUserData),
      _pClient(&client),
      _id(id),
      _index(static_cast<uint32_t>(client._connections.size())) {
  setHandlerCallbacks();
}

WebSocketConnection::~WebSocketConnection() = default;

void WebSocketConnection::setHandlerCallbacks() {
  // The handler only knows its own callbacks: they forward to the ones of the connection, given with it. It processes
  // nothing after the Close frame of the server, nor after a failure: a message or a Pong following them in the same
  // read is not delivered.
  websocket::WebSocketCallbacks handlerCallbacks{
      .onMessage = {},  // set below, like onPong
      .onPing = {},
      .onPong = {},
      .onClose = [this](websocket::CloseCode code,
                        std::string_view reason) { notifyClose(code, reason, State::Draining); },
      // The handler then sends its Close frame, and processInput() ends the connection.
      .onError = [this](websocket::CloseCode code,
                        std::string_view message) { notifyClose(code, message, State::Draining); },
  };
  if (_callbacks.onMessage != nullptr) {
    handlerCallbacks.onMessage = [this](std::span<const std::byte> payload, bool isBinary) {
      _callbacks.onMessage(*this, payload, isBinary);
    };
  } else {
    handlerCallbacks.onMessage = [this](std::span<const std::byte> payload, bool isBinary) { keep(payload, isBinary); };
  }
  if (_callbacks.onPong != nullptr) {
    handlerCallbacks.onPong = [this](std::span<const std::byte> payload) { _callbacks.onPong(*this, payload); };
  }
  _handler.setCallbacks(std::move(handlerCallbacks));
}

void WebSocketConnection::startSession(UpgradedConnection&& upgraded,
                                       [[maybe_unused]] const websocket::UpgradeResponseValidation& validation) {
#ifdef AERONET_ENABLE_ZLIB
  if (validation.deflateParams.hasValue()) {
    _handler.enableCompression(validation.deflateParams);
  }
#endif
  _transport = std::move(upgraded.transport);
  _in = std::move(upgraded.received);
  _cnx = std::move(upgraded.cnx);
  const auto now = SteadyClock::now();
  _lastActivity = now;
  _openedAt = now;
  _pendingInput = !_in.empty() || _transport.hasPendingReadData();
  _unprocessedInput = !_in.empty();
  _writeFailed = false;
  _writeBlocked = false;
  _writeWantsRead = false;
  _readWantsWrite = false;
  _reconnecting = false;
  _state = State::Open;
}

bool WebSocketConnection::close(websocket::CloseCode code, std::string_view reason) {
  if (_state == State::Open) {
    if (!_handler.sendClose(code, reason)) {
      return false;  // already closing
    }
    _closeRequested = true;
    return queued(true);
  }
  if (_state != State::Waiting && _state != State::Connecting && !_reconnecting) {
    return false;  // closing or ended
  }
  // No connection to close: the next attempt is cancelled.
  _closeRequested = true;
  _reconnecting = false;
  if (_state != State::Draining && _state != State::Ended) {
    _pClient->abortHandshake(*this);
    _state = State::Ended;
  }
  return true;
}

bool WebSocketConnection::queued(bool ok) {
  // From a callback, poll() writes the frames once done with the received bytes. Otherwise they are written now, unless
  // the socket already refused bytes: poll() writes them all when it becomes writable.
  if (ok && !_pClient->_inPoll && !_writeBlocked && !flush()) {
    // The connection is lost: the next poll() ends it at once (onClose is not called from a send).
    _writeFailed = true;
  }
  return ok;
}

bool WebSocketConnection::flush() {
#ifdef AERONET_LINUX
  // OpenSSL writes on the socket itself, which raises SIGPIPE once the server reset the connection. Only blocked when
  // writing: a connection that only receives makes no system call for it.
  const SigpipeBlocker sigpipeBlocker(_transport.isTls() && _handler.pendingOutputSize() != 0);
#endif
  vector<std::string_view>& fragments = _pClient->_fragments;
  _writeBlocked = false;
  _writeWantsRead = false;
  while (_handler.pendingOutputSize() != 0) {
    // The buffered frames, then the payload sent from where it is (a large compressed message): a single write call.
    _handler.getPendingOutputFragments(fragments);
    const TransportResult res = _transport.write(std::span<const std::string_view>(fragments));
    _handler.onOutputWritten(res.bytesProcessed);
    if (res.want == TransportHint::Error) {
      return false;
    }
    if (res.want != TransportHint::None) {
      // The socket takes no more bytes for now (written when it is writable), or TLS needs to read first (written when
      // the socket is readable, and by each poll()).
      _writeBlocked = res.want == TransportHint::WriteReady;
      _writeWantsRead = res.want == TransportHint::ReadReady;
      break;
    }
  }
  return updateInterest();
}

EventBmp WebSocketConnection::sessionInterest() const noexcept {
  EventBmp interest = _writeBlocked ? EventOut : 0;
  if (!_paused) {
    interest |= kReadEvents;
    if (_readWantsWrite) {
      interest |= EventOut;
    }
  } else if (_writeWantsRead) {
    interest |= EventIn;  // not read for messages: the write, retried, lets TLS read what it waits for
  }
  return interest;
}

bool WebSocketConnection::updateInterest() { return _pClient->watch(*this, _cnx.fd(), sessionInterest()); }

bool WebSocketConnection::setReadWantsWrite(bool readWantsWrite) {
  if (_readWantsWrite == readWantsWrite) {
    return true;
  }
  _readWantsWrite = readWantsWrite;
  return updateInterest();
}

void WebSocketConnection::read(SteadyClock::time_point now) {
  if (_writeFailed) {
    lose("connection error");
    return;
  }
  if (_unprocessedInput) {
    // The bytes received with the 101 response, before any read.
    _unprocessedInput = false;
    processInput();
    if (_state == State::Ended) {
      return;
    }
    if (_paused) {
      pauseReads();
      return;
    }
  }
  const uint32_t readChunkBytes = _pClient->_readChunkBytes;
  for (uint32_t nbReads = 0; nbReads < kMaxReadsPerPoll; ++nbReads) {
    // A large frame being received makes the buffer grow exponentially: each read fills all of its free capacity.
    _in.ensureAvailableCapacityExponential(readChunkBytes);
    const std::size_t available = _in.capacity() - _in.size();
    const TransportResult res = _transport.read(_in.data() + _in.size(), available);
    if (res.bytesProcessed == 0) {
      if (res.want == TransportHint::ReadReady || res.want == TransportHint::WriteReady) {
        // TLS may have to write before it reads again (the answer to a TLS 1.3 KeyUpdate, while the socket takes no
        // more bytes): the read is then retried once the socket is writable.
        _pendingInput = false;
        if (!setReadWantsWrite(res.want == TransportHint::WriteReady)) {
          lose("connection error");
        }
        return;
      }
      if (_state == State::Draining) {
        _state = State::Ended;  // the server closed the connection after the closing handshake, as expected
      } else {
        lose(res.want == TransportHint::Error ? "connection error" : "connection closed by the server");
      }
      return;
    }
    if (_readWantsWrite && !setReadWantsWrite(false)) [[unlikely]] {
      lose("connection error");
      return;
    }
    _in.addSize(res.bytesProcessed);
    _lastActivity = now;
    if (_state == State::Open) {
      processInput();
      if (_state == State::Ended) {
        return;
      }
      if (_paused) {
        pauseReads();
        return;
      }
    } else {
      _in.clear();  // received after the Close frame of the server: discarded
    }
    if (res.bytesProcessed < available && !_transport.hasPendingReadData()) {
      _pendingInput = false;  // the socket is drained, its readiness tells when more bytes come
      return;
    }
  }
  // Read budget used: the next poll() reads the rest without waiting (TLS may hold bytes the socket does not signal).
  _pendingInput = true;
}

void WebSocketConnection::processInput() {
  // Only complete frames are consumed: an incomplete one stays at the start of the buffer, completed by the next reads.
  // Should a callback throw, the bytes already processed are unknown: poll() then ends the connection.
  const ProtocolProcessResult res = _handler.processInput(std::as_bytes(std::span<const char>(_in.data(), _in.size())));
  _in.erase_front(res.bytesConsumed);
  if (res.action == ProtocolProcessResult::Action::Close && !_handler.isCloseComplete()) {
    // The client failed the connection (onClose was called with the code of its Close frame): the TCP connection is
    // closed once that frame is written, without waiting for the answer of the server (RFC 6455 §7.1.7).
    if (!flush()) {
      log::warn("WebSocket client: the Close frame failing the connection of fd # {} could not be written", _cnx.fd());
    }
    _state = State::Ended;
  }
}

void WebSocketConnection::keep(std::span<const std::byte> payload, bool isBinary) {
  ReceiveQueue& queue = *_pReceiveQueue;
  queue.bytes.append(payload);
  queue.entries.emplace_back(payload.size(), isBinary);
  _paused = true;
}

void WebSocketConnection::pauseReads() {
  _pendingInput = true;  // read again once the kept messages were received
  if (!updateInterest()) {
    lose("connection error");
  }
}

void WebSocketConnection::resume(SteadyClock::time_point now) {
  _paused = false;
  if (!inSession()) {
    return;  // the next session watches its socket
  }
  _lastActivity = now;   // the time paused is not idle time
  _pendingInput = true;  // TLS may hold bytes that the socket does not signal
  if (!updateInterest()) {
    _writeFailed = true;  // the next poll() ends the connection
  }
}

WebSocketReceiveResult WebSocketConnection::receive(std::chrono::milliseconds timeout) {
  WebSocketClient& client = *_pClient;
  if (client._inPoll) [[unlikely]] {
    ThrowCalledFromCallback();
  }
  if (_pReceiveQueue == nullptr) [[unlikely]] {
    throw std::logic_error("WebSocketConnection::receive() needs a connection without onMessage callback");
  }
  ReceiveQueue& queue = *_pReceiveQueue;
  auto now = SteadyClock::now();
  if (queue.empty() && !queue.entries.empty()) {
    // All the kept messages were returned (the last one, by the previous call, is now dropped): read again.
    queue.bytes.clear();
    queue.entries.clear();
    queue.offset = 0;
    queue.next = 0;
    resume(now);
  }
  const auto waiting = [this, &queue] { return queue.empty() && _state != State::Ended; };
  if (waiting()) {
    // One poll at least, also with a zero timeout: it reads what the socket already holds.
    const SteadyClock::time_point deadline = DeadlineAfter(now, timeout);
    do {
      client.poll(std::chrono::ceil<std::chrono::milliseconds>(deadline - now));
      now = SteadyClock::now();
    } while (waiting() && now < deadline);
    if (waiting()) {
      return {};
    }
  }
  WebSocketReceiveResult result;
  if (!queue.empty()) {
    const ReceiveQueue::Entry& entry = queue.entries[queue.next];
    result.payload = std::span<const std::byte>(queue.bytes.data() + queue.offset, entry.size);
    result.isBinary = entry.isBinary;
    result.status = WebSocketReceiveResult::Status::Message;
    queue.offset += entry.size;
    ++queue.next;
    return result;
  }
  _endReported = true;  // destroyed by the next poll()
  result.closeCode = queue.closeCode;
  result.closeReason = queue.closeReason;
  result.status = WebSocketReceiveResult::Status::Closed;
  return result;
}

const HttpResponse* WebSocketConnection::handshakeResponse() const noexcept {
  return _handshake != nullptr && _handshake->phase() == internal::UpgradeHandshake::Phase::Done
             ? &_handshake->response()
             : nullptr;
}

HttpClientErrc WebSocketConnection::handshakeError() const noexcept {
  return _handshake != nullptr ? _handshake->error() : HttpClientErrc::noError;
}

void WebSocketConnection::notifyClose(websocket::CloseCode code, std::string_view reason, State nextState) {
  const bool open = _state == State::Open;
  if (open || nextState == State::Ended) {
    _state = nextState;  // a draining connection can only end, onClose was already called
  }
  if (nextState == State::Ended) {
    _handler.discardPendingOutput();  // never written: bufferedAmount() is 0, onClose included
  }
  if (!open) {
    return;
  }
  const auto now = SteadyClock::now();
  if (nextState == State::Draining) {
    // The server closes the TCP connection right after the closing handshake (RFC 6455 §7.1.1): it is closed after
    // closeTimeout otherwise.
    _stateDeadline = DeadlineAfter(now, _handler.config().closeTimeout);
  }
  const RetryConfig& retry = _pClient->_reconnect.retry;
  if (now - _openedAt >= retry.maxDelay) {
    _attempt = 1;  // a stable connection: the attempts start again, from the handshake of this session
  }
  _reconnecting = !_closeRequested && _attempt < retry.maxAttempts;
  if (_pReceiveQueue != nullptr) {
    _pReceiveQueue->closeCode = code;
    _pReceiveQueue->closeReason.assign(reason);
  }
  if (_callbacks.onClose != nullptr) {
    _callbacks.onClose(*this, code, reason);
  }
}

void WebSocketConnection::writePending(bool writable) {
  if (inSession() && _handler.pendingOutputSize() != 0 && (writable || !_writeBlocked) && !flush()) {
    lose("connection error");
  }
}

SteadyClock::time_point WebSocketConnection::nextDeadline(SteadyClock::time_point now) const noexcept {
  switch (_state) {
    case State::Waiting:
      [[fallthrough]];
    case State::Draining:
      return _stateDeadline;
    case State::Connecting:
      return _handshake->deadline();
    case State::Open:
      return _handler.nextTimeoutCheck(lastActivity(now));
    default:
      return SteadyClock::time_point::max();
  }
}

void WebSocketConnection::checkTimeouts(SteadyClock::time_point now) {
  if (_state == State::Draining) {
    if (now >= _stateDeadline) {
      _state = State::Ended;
    }
    return;
  }
  // A Ping queued for the idle connection is written with the pending output.
  switch (_handler.checkTimeouts(now, lastActivity(now))) {
    case websocket::WebSocketHandler::Timeout::Idle:
      lose("idle timeout");
      break;
    case websocket::WebSocketHandler::Timeout::Close:
      lose("no Close frame from the server within closeTimeout");
      break;
    default:
      break;
  }
}

WebSocketClient::WebSocketClient() : WebSocketClient(websocket::WebSocketConfig{}) {}

WebSocketClient::WebSocketClient(websocket::WebSocketConfig config, HttpClientConfig httpConfig,
                                 WebSocketReconnectConfig reconnectConfig)
    : _config(ClientSideConfig(std::move(config))),
      _reconnect(ValidatedReconnectConfig(std::move(reconnectConfig))),
      _http(std::move(httpConfig)),
      _loop(PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{1}}, kNbInitialEvents),
      _shared(std::make_unique<internal::WebSocketClientShared>()),
      _readChunkBytes(_http.config().minReadChunkBytes) {
  _loop.addOrThrow(EventLoop::EventFd{_shared->wakeup.fd(), EventIn});
}

WebSocketClient::WebSocketClient(WebSocketClient&& rhs) noexcept
    : _config(std::move(rhs._config)),
      _reconnect(std::move(rhs._reconnect)),
      _http(std::move(rhs._http)),
      _loop(std::move(rhs._loop)),
      _connections(std::move(rhs._connections)),
      _fds(std::move(rhs._fds)),
      _fragments(std::move(rhs._fragments)),
      _hostAttempts(std::move(rhs._hostAttempts)),
      _shared(std::move(rhs._shared)),
      _nextId(rhs._nextId),
      _readChunkBytes(rhs._readChunkBytes),
      _pollTimeoutMs(rhs._pollTimeoutMs) {
  assert(!rhs._inPoll);
  for (const auto& pCnx : _connections) {
    pCnx->_pClient = this;
  }
}

WebSocketClient& WebSocketClient::operator=(WebSocketClient&& rhs) noexcept {
  if (this != &rhs) {
    assert(!_inPoll && !rhs._inPoll);
    // The connections of this client close with their sockets, which leave the event loop by themselves.
    _connections = std::move(rhs._connections);
    _fds = std::move(rhs._fds);
    _config = std::move(rhs._config);
    _reconnect = std::move(rhs._reconnect);
    _http = std::move(rhs._http);
    _loop = std::move(rhs._loop);
    _fragments = std::move(rhs._fragments);
    _hostAttempts = std::move(rhs._hostAttempts);
    _shared = std::move(rhs._shared);
    _nextId = rhs._nextId;
    _readChunkBytes = rhs._readChunkBytes;
    _pollTimeoutMs = rhs._pollTimeoutMs;
    for (const auto& pCnx : _connections) {
      pCnx->_pClient = this;
    }
  }
  return *this;
}

WebSocketClient::~WebSocketClient() = default;

WebSocketConnection& WebSocketClient::addConnection(HttpRequest&& upgradeRequest,
                                                    const WebSocketClientCallbacks& callbacks, void* pUserData) {
  // The opening handshake is a GET (RFC 6455 §4.1), in which a client should not send content (RFC 9110 §9.3.1). A
  // request without body has no trailers either, as required by UpgradeHandshake.
  if (upgradeRequest.method() != http::Method::GET) {
    throw std::invalid_argument("a WebSocket upgrade request must be a GET");
  }
  if (upgradeRequest.hasBody()) {
    throw std::invalid_argument("a WebSocket upgrade request cannot have a body");
  }
  if (upgradeRequest.isTlsRequest()) {
#ifdef AERONET_ENABLE_OPENSSL
    // Built (and validated) once: an invalid TLS configuration throws here rather than in poll().
    _http.tlsContext();
#else
    throw std::logic_error("https requested but aeronet was built without OpenSSL support");
#endif
  }
  upgradeRequest.headerAddLine(websocket::SecWebSocketVersion, websocket::kWebSocketVersion);
#ifdef AERONET_ENABLE_ZLIB
  if (_config.deflateConfig.enabled) {
    upgradeRequest.headerInlineAddLine(
        websocket::SecWebSocketExtensions, websocket::kMaxDeflateOfferSize, [this](char* out) -> std::size_t {
          return static_cast<std::size_t>(websocket::BuildDeflateOffer(_config.deflateConfig, out) - out);
        });
  }
#endif
  // Room in both vectors first: the connection is added to both (at the same index) or to none.
  ReserveOneMore(_connections);
  ReserveOneMore(_fds);
  auto pCnx = std::make_unique<WebSocketConnection>(
      WebSocketConnection::PrivateTag{}, *this, std::make_unique<internal::UpgradeHandshake>(std::move(upgradeRequest)),
      callbacks, pUserData, _nextId);
  ++_nextId;
  _fds.push_back(kInvalidHandle);
  return *_connections.emplace_back(std::move(pCnx));
}

void WebSocketClient::removeConnection(uint32_t pos) {
  abortHandshake(*_connections[pos]);
  _fds[pos] = _fds.back();
  _fds.pop_back();
  std::swap(_connections[pos], _connections.back());
  _connections.pop_back();  // closes the connection
  if (pos < _connections.size()) {
    _connections[pos]->_index = pos;
  }
}

WebSocketConnectResult WebSocketClient::connect(HttpRequest&& upgradeRequest, const WebSocketClientCallbacks& callbacks,
                                                void* pUserData) {
  if (_inPoll) [[unlikely]] {
    // The events being processed by poll() could refer to a moved buffer.
    ThrowCalledFromCallback();
  }
  WebSocketConnection& cnx = addConnection(std::move(upgradeRequest), callbacks, pUserData);
  const uint32_t pos = cnx._index;
  WebSocketConnectResult result;
  try {
    if (const auto now = SteadyClock::now(), slot = connectSlot(cnx, now); slot > now) {
      std::this_thread::sleep_for(slot - now);  // minConnectInterval
    }
    internal::UpgradeHandshake& handshake = *cnx._handshake;
    cnx._key = websocket::GenerateWebSocketKey();
    const KeyLine keyLine = MakeKeyLine(cnx._key);
    ++cnx._attempt;
    cnx._state = State::Connecting;
    recordConnectAttempt(cnx);
    handshake.run(_http, std::string_view(keyLine.data(), keyLine.size()));

    if (handshake.phase() == internal::UpgradeHandshake::Phase::Failed) {
      result.httpError = handshake.error();
      result.pErrorMessage = ErrcToStr(handshake.error()).data();
    } else if (handshake.response().status() != http::StatusCodeSwitchingProtocols) {
      result.pErrorMessage = kNotSwitched;
    } else if (const websocket::UpgradeResponseValidation validation = ValidateHandshake(handshake, cnx._key, _config);
               !validation.valid()) {
      result.pErrorMessage = validation.pErrorMessage;
    } else {
      cnx.startSession(handshake.takeConnection(), validation);
      if (!watch(cnx, cnx._cnx.fd(), cnx.sessionInterest())) {
        result.httpError = HttpClientErrc::ioError;
        result.pErrorMessage = ErrcToStr(HttpClientErrc::ioError).data();
      } else {
        result.pConnection = &cnx;
        if (callbacks.onOpen != nullptr) {
          callbacks.onOpen(cnx);  // the frames it sends are written at once
        }
      }
    }
    if (handshake.phase() == internal::UpgradeHandshake::Phase::Done) {
      result.response = std::move(handshake.response());
    }
    if (result.pConnection == nullptr) {
      removeConnection(pos);  // also when the connection would reconnect: connect() does not retry
    } else if (!_reconnect.enabled()) {
      cnx._handshake.reset();  // no other handshake
    } else {
      handshake.reset();  // kept for the reconnections, with its request
    }
  } catch (...) {
    // A throwing onOpen: the caller never gets the connection. It ends at once, like an invalid TLS configuration.
    removeConnection(pos);
    throw;
  }
  return result;
}

WebSocketConnection& WebSocketClient::connectAsync(HttpRequest&& upgradeRequest,
                                                   const WebSocketClientCallbacks& callbacks, void* pUserData) {
  WebSocketConnection& cnx = addConnection(std::move(upgradeRequest), callbacks, pUserData);
  scheduleHandshake(cnx, std::chrono::milliseconds::zero());
  return cnx;
}

SteadyClock::time_point WebSocketClient::connectSlot(const WebSocketConnection& cnx,
                                                     SteadyClock::time_point earliest) const noexcept {
  const std::string_view host = cnx._handshake->request().host();
  const auto it = std::ranges::find_if(
      _hostAttempts, [host](const HostAttempt& attempt) { return CaseInsensitiveEqual(attempt.host, host); });
  return it == _hostAttempts.end() ? earliest
                                   : std::max(earliest, DeadlineAfter(it->time, _reconnect.minConnectInterval));
}

void WebSocketClient::recordConnectAttempt(const WebSocketConnection& cnx) {
  const std::chrono::milliseconds interval = _reconnect.minConnectInterval;
  if (interval == std::chrono::milliseconds::zero()) {
    return;  // nothing to space out
  }
  const auto now = SteadyClock::now();
  // The attempts imposing nothing anymore are forgotten.
  const auto [first, last] = std::ranges::remove_if(_hostAttempts, [now, interval](const HostAttempt& attempt) {
    return DeadlineAfter(attempt.time, interval) <= now;
  });
  _hostAttempts.erase(first, last);
  // The last attempt to this host started minConnectInterval ago at least (connectSlot()): it was forgotten above.
  assert(connectSlot(cnx, now) == now);
  _hostAttempts.emplace_back(RawChars32(cnx._handshake->request().host()), now);
}

void WebSocketClient::scheduleHandshake(WebSocketConnection& cnx, std::chrono::milliseconds delay) {
  cnx._state = State::Waiting;
  // Checked again when the wait ends: other attempts to the host may start meanwhile.
  cnx._stateDeadline = connectSlot(cnx, DeadlineAfter(SteadyClock::now(), delay));
}

void WebSocketClient::startHandshake(WebSocketConnection& cnx) {
  internal::UpgradeHandshake& handshake = *cnx._handshake;
  cnx._key = websocket::GenerateWebSocketKey();
  const KeyLine keyLine = MakeKeyLine(cnx._key);
  ++cnx._attempt;
  cnx._state = State::Connecting;
  recordConnectAttempt(cnx);
  handshake.start(_http, std::string_view(keyLine.data(), keyLine.size()));
  const std::span<char> host = handshake.hostToResolve(_http);
  const std::string_view hostView(host.data(), host.size());
  const uint16_t port = handshake.portToResolve(_http);
  if (IsNumericHost(hostView)) {
    handshake.onResolved(_http, ResolveTCP(host, port));  // no query
  } else {
    // getaddrinfo can block for seconds: the other connections are served meanwhile.
    cnx._resolution = std::make_shared<internal::HostResolution>(hostView, port);
    _shared->resolver.resolve(cnx._resolution);
  }
  handshakeStepped(cnx);
}

void WebSocketClient::handshakeStepped(WebSocketConnection& cnx) {
  internal::UpgradeHandshake& handshake = *cnx._handshake;
  switch (handshake.phase()) {
    case internal::UpgradeHandshake::Phase::Done:
      handshakeDone(cnx);
      break;
    case internal::UpgradeHandshake::Phase::Failed:
      handshakeFailed(cnx, ErrcToStr(handshake.error()), backoffDelay(cnx));
      break;
    case internal::UpgradeHandshake::Phase::Resolve:
      break;  // a thread resolves the host
    default:
      if (!watch(cnx, handshake.fd(), handshake.interest())) {
        handshake.fail(HttpClientErrc::ioError);
        handshakeFailed(cnx, ErrcToStr(HttpClientErrc::ioError), backoffDelay(cnx));
      }
      break;
  }
}

void WebSocketClient::handshakeDone(WebSocketConnection& cnx) {
  internal::UpgradeHandshake& handshake = *cnx._handshake;
  const HttpResponse& response = handshake.response();
  if (response.status() != http::StatusCodeSwitchingProtocols) {
    // A refusal: only retried for the statuses of retryStatuses, after their Retry-After delay.
    const RetryConfig& retry = _reconnect.retry;
    handshakeFailed(cnx, kNotSwitched,
                    retry.retryStatuses.contains(response.status())
                        ? retry.statusRetryDelay(response.headerValueOrEmpty(http::RetryAfter), backoffDelay(cnx))
                        : kNoRetry);
    return;
  }
  const websocket::UpgradeResponseValidation validation = ValidateHandshake(handshake, cnx._key, _config);
  if (!validation.valid()) {
    handshakeFailed(cnx, validation.pErrorMessage, kNoRetry);  // a server not speaking the protocol as offered
    return;
  }
  cnx.startSession(handshake.takeConnection(), validation);
  if (!cnx.updateInterest()) {
    cnx._writeFailed = true;  // the next poll() ends the connection
  }
  if (cnx._callbacks.onOpen != nullptr) {
    cnx._callbacks.onOpen(cnx);  // the frames it sends are written with the others of this poll()
  }
  if (!_reconnect.enabled()) {
    cnx._handshake.reset();  // no other handshake
  } else {
    handshake.reset();  // kept for the reconnections, with its request
  }
}

void WebSocketClient::handshakeFailed(WebSocketConnection& cnx, std::string_view reason,
                                      std::chrono::milliseconds retryDelay) {
  log::warn("WebSocket client: opening handshake with {} failed: {}", cnx._handshake->request().originKey(), reason);
  unwatch(cnx);  // the socket of the attempt is closed below
  if (cnx._resolution != nullptr) {
    _shared->resolver.cancel(*cnx._resolution);
    cnx._resolution.reset();
  }
  cnx._reconnecting = retryDelay >= std::chrono::milliseconds::zero() && !cnx._closeRequested &&
                      cnx._attempt < _reconnect.retry.maxAttempts;
  cnx._state = State::Ended;
  if (cnx._pReceiveQueue != nullptr) {
    cnx._pReceiveQueue->closeCode = websocket::CloseCode::AbnormalClosure;
    cnx._pReceiveQueue->closeReason.assign(reason);
  }
  if (cnx._callbacks.onClose != nullptr) {
    cnx._callbacks.onClose(cnx, websocket::CloseCode::AbnormalClosure, reason);
  }
  cnx._handshake->reset();
  if (cnx._reconnecting) {
    scheduleHandshake(cnx, retryDelay);
  }
}

void WebSocketClient::abortHandshake(WebSocketConnection& cnx) {
  if (cnx._resolution != nullptr) {
    _shared->resolver.cancel(*cnx._resolution);
    cnx._resolution.reset();
  }
  unwatch(cnx);
  if (cnx._handshake != nullptr) {
    cnx._handshake->reset();
  }
}

void WebSocketClient::sessionEnded(WebSocketConnection& cnx) {
  if (cnx._cnx) {
    // The socket of the session closes.
    unwatch(cnx);
    cnx._transport = Transport();
    cnx._cnx = Connection();
    cnx._in = RawChars();
  }
  if (!cnx._reconnecting) {
    return;  // the end: destroyed, or kept until receive() returns it
  }
  // A fresh protocol state for the next session, releasing the buffers of this one.
  cnx._handler = websocket::WebSocketHandler(_config);
  cnx.setHandlerCallbacks();
  scheduleHandshake(cnx, backoffDelay(cnx));
}

std::chrono::milliseconds WebSocketClient::backoffDelay(const WebSocketConnection& cnx) {
  const RetryConfig& retry = _reconnect.retry;
  return retry.delayFor(cnx._attempt - 1U, retry.jitter > 0.0F ? _http.nextJitterUnit() : 0.0);
}

void WebSocketClient::serveWakeup() {
  internal::WebSocketClientShared& shared = *_shared;
  // Drained first: a thread signaling after the takes below wakes the next poll() up.
  shared.wakeup.read();

  if (shared.resolver.hasResolved()) {
    shared.resolver.takeResolved(shared.resolved);  // after those left by a callback that threw
  }
  // Each one removed before it is served: should a callback throw, the next ones stay for the next poll().
  while (!shared.resolved.empty()) {
    const std::shared_ptr<internal::HostResolution> resolution = std::move(shared.resolved.back());
    shared.resolved.pop_back();
    const auto it =
        std::ranges::find_if(_connections, [&resolution](const auto& pCnx) { return pCnx->_resolution == resolution; });
    if (it == _connections.end()) {
      continue;  // its handshake ended meanwhile
    }
    WebSocketConnection& cnx = **it;
    _pServed = &cnx;
    cnx._resolution.reset();
    cnx._handshake->onResolved(_http, std::move(resolution->addresses));
    handshakeStepped(cnx);
  }

  // Emptied before taking the posted frames rather than after sending them: should a send throw (ending its
  // connection), the frames taken after it are dropped here instead of being swapped back and sent again.
  shared.taken.clear();
  shared.takenBytes.clear();
  {
    const std::scoped_lock lock(shared.mutex);
    if (shared.posted.empty()) {
      return;
    }
    shared.posted.swap(shared.taken);
    shared.postedBytes.swap(shared.takenBytes);
  }
  std::size_t offset = 0;
  for (const internal::WebSocketClientShared::Posted& posted : shared.taken) {
    const std::span<const std::byte> payload(shared.takenBytes.data() + offset, posted.size);
    offset += posted.size;
    WebSocketConnection* pCnx = findConnection(posted.id);
    if (pCnx == nullptr) {
      continue;
    }
    _pServed = pCnx;
    switch (posted.kind) {
      case internal::WebSocketClientShared::Posted::Kind::Text:
        pCnx->sendText(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
        break;
      case internal::WebSocketClientShared::Posted::Kind::Binary:
        pCnx->sendBinary(payload);
        break;
      default:
        pCnx->close(posted.closeCode, std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
        break;
    }
  }
}

bool WebSocketClient::readsWithoutWaiting(SteadyClock::time_point now,
                                          SteadyClock::time_point& deadline) const noexcept {
  bool readNow = false;
  for (const auto& pCnx : _connections) {
    readNow |= pCnx->inSession() && ((pCnx->_pendingInput && !pCnx->_paused) || pCnx->_writeFailed);
    deadline = std::min(deadline, pCnx->nextDeadline(now));
  }
  return readNow;
}

std::size_t WebSocketClient::poll(std::chrono::milliseconds timeout) {
  if (_inPoll) [[unlikely]] {
    ThrowCalledFromCallback();
  }
  // The callbacks see it set (their sends are written below, together), also when one throws.
  struct InPollScope {
    explicit InPollScope(bool& inPoll) noexcept : _inPoll(inPoll) { _inPoll = true; }

    InPollScope(const InPollScope&) = delete;
    InPollScope(InPollScope&&) = delete;
    InPollScope& operator=(const InPollScope&) = delete;
    InPollScope& operator=(InPollScope&&) = delete;

    ~InPollScope() { _inPoll = false; }

    bool& _inPoll;
  };
  const InPollScope inPollScope(_inPoll);

  SteadyClock::time_point now = SteadyClock::now();
  SteadyClock::time_point deadline =
      now + std::min(timeout, std::chrono::milliseconds{std::numeric_limits<int>::max()});
  std::span<const EventLoop::EventFd> events;
  if (readsWithoutWaiting(now, deadline)) {
    events = _loop.poll(false);  // bytes are already there: only collect the events ready
  } else {
    if (const int timeoutMs = PollTimeoutMs(now, deadline); timeoutMs != _pollTimeoutMs) {
      _loop.updatePollTimeoutPolicy(PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{timeoutMs}});
      _pollTimeoutMs = timeoutMs;
    }
    events = _loop.poll();
  }
  now = SteadyClock::now();

  _pServed = nullptr;  // the connection being served, ended if a callback throws
  try {
    bool wakeup = false;
    const NativeHandle wakeupFd = _shared->wakeup.fd();
    for (const EventLoop::EventFd& event : events) {
      if (event.fd == wakeupFd) {
        wakeup = true;
        continue;
      }
      WebSocketConnection* pCnx = find(event.fd);
      if (pCnx == nullptr) {
        continue;  // no longer watched: its handshake was stopped by a callback
      }
      _pServed = pCnx;
      WebSocketConnection& cnx = *pCnx;
      if (cnx._state == State::Connecting) {
        internal::UpgradeHandshake& handshake = *cnx._handshake;
        if (handshake.mayReplaceSocket()) {
          unwatch(cnx);
        }
        handshake.advance(_http, event.eventBmp);
        handshakeStepped(cnx);
        continue;
      }
      if ((event.eventBmp & EventOut) != 0) {
        cnx.writePending(true);
      }
      // Readable, closed or failed: the read tells, as it does once writable for a TLS read that had to write first. A
      // paused connection is only read once closed or failed: its readability is watched for a TLS write, written
      // below. kqueue (macOS) reports the readability and the writability of a socket as two events: an ended
      // connection is not read anymore.
      const EventBmp readEvents = event.eventBmp & (cnx._paused ? (EventErr | EventHup) : ~EventOut);
      if ((readEvents != 0 || cnx._readWantsWrite) && cnx.inSession()) {
        cnx.read(now);
      }
    }
    if (wakeup) {
      serveWakeup();
    }

    // Index based: the callbacks may add connections (connectAsync()), which are served too.
    for (uint32_t pos = 0; pos < _connections.size(); ++pos) {
      _pServed = _connections[pos].get();
      WebSocketConnection& cnx = *_pServed;
      switch (cnx._state) {
        case State::Waiting:
          if (now >= cnx._stateDeadline) {
            // The wait goes on after an attempt to the host started since it began, also earlier in this pass: the
            // attempts due together (polls coming late) start one after the other.
            cnx._stateDeadline = connectSlot(cnx, now);
            if (cnx._stateDeadline <= now) {
              startHandshake(cnx);
            }
          }
          break;
        case State::Connecting:
          if (now >= cnx._handshake->deadline()) {
            cnx._handshake->expire();
            handshakeStepped(cnx);
          }
          break;
        case State::Open:
          [[fallthrough]];
        case State::Draining:
          if ((cnx._pendingInput && !cnx._paused) || cnx._writeFailed) {
            cnx.read(now);  // bytes held by TLS, or left by the read budget
          }
          if (cnx._state != State::Ended) {
            cnx.checkTimeouts(now);
          }
          // The frames queued by the callbacks (messages, Pong, Close, Ping of an idle connection), written together.
          cnx.writePending(false);
          break;
        default:
          break;
      }
    }
    // The callbacks of the last connections may have queued frames on the first ones. Index based too: a failed write
    // runs onClose, which may add connections.
    for (uint32_t pos = 0; pos < _connections.size(); ++pos) {
      _pServed = _connections[pos].get();
      _pServed->writePending(false);
    }
  } catch (...) {
    // A callback of the served connection threw (or one of its buffers could not grow): the bytes it processed are
    // unknown, so it ends at once, without onClose nor reconnection. The others carry on with the next poll().
    if (_pServed != nullptr) {
      _pServed->_state = State::Ended;
      _pServed->_reconnecting = false;
      _pServed->_endReported = true;
    }
    if (!_shared->resolved.empty()) {
      _shared->wakeup.send();  // the resolutions taken with the one whose callback threw: served by the next poll()
    }
    removeEndedConnections();
    throw;
  }

  removeEndedConnections();
  return nbConnections();
}

std::size_t WebSocketClient::nbConnections() const noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(
      _connections, [](const auto& pCnx) { return pCnx->_state != State::Ended || pCnx->_reconnecting; }));
}

WebSocketConnection* WebSocketClient::findConnection(uint64_t id) const noexcept {
  const auto it = std::ranges::find_if(_connections, [id](const auto& pCnx) { return pCnx->_id == id; });
  return it == _connections.end() || (*it)->_state == State::Ended ? nullptr : it->get();
}

void WebSocketClient::postText(uint64_t id, std::string_view text) {
  _shared->post(id, internal::WebSocketClientShared::Posted::Kind::Text, std::as_bytes(std::span(text)),
                websocket::CloseCode::Normal);
}

void WebSocketClient::postBinary(uint64_t id, std::span<const std::byte> data) {
  _shared->post(id, internal::WebSocketClientShared::Posted::Kind::Binary, data, websocket::CloseCode::Normal);
}

void WebSocketClient::postClose(uint64_t id, websocket::CloseCode code, std::string_view reason) {
  _shared->post(id, internal::WebSocketClientShared::Posted::Kind::Close, std::as_bytes(std::span(reason)), code);
}

void WebSocketClient::wakeup() const noexcept { _shared->wakeup.send(); }

std::chrono::milliseconds WebSocketClient::nextTimeout() const noexcept {
  const auto now = SteadyClock::now();
  SteadyClock::time_point deadline = SteadyClock::time_point::max();
  if (readsWithoutWaiting(now, deadline) || deadline <= now) {
    return std::chrono::milliseconds::zero();
  }
  if (deadline == SteadyClock::time_point::max()) {
    return std::chrono::milliseconds::max();
  }
  return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
}

WebSocketConnection* WebSocketClient::find(NativeHandle fd) const noexcept {
  const auto it = std::ranges::find(_fds, fd);
  return it == _fds.end() ? nullptr : _connections[static_cast<uint32_t>(it - _fds.begin())].get();
}

bool WebSocketClient::watch(WebSocketConnection& cnx, NativeHandle fd, EventBmp interest) {
  if (cnx._interest == interest) {
    return true;
  }
  if (interest == 0) {
    unwatch(cnx);
    return true;
  }
  if (cnx._interest == 0) {
    if (!_loop.add(EventLoop::EventFd{fd, interest})) {
      return false;
    }
    _fds[cnx._index] = fd;
  } else if (!_loop.mod(EventLoop::EventFd{fd, interest})) {
    return false;
  }
  cnx._interest = interest;
  return true;
}

void WebSocketClient::unwatch(WebSocketConnection& cnx) noexcept {
  if (cnx._interest != 0) {
    _loop.del(_fds[cnx._index]);
    _fds[cnx._index] = kInvalidHandle;
    cnx._interest = 0;
  }
}

void WebSocketClient::removeEndedConnections() {
  for (uint32_t pos = 0; pos < _connections.size();) {
    WebSocketConnection& cnx = *_connections[pos];
    if (cnx._state == State::Ended) {
      sessionEnded(cnx);
    }
    if (cnx._state != State::Ended || (cnx._pReceiveQueue != nullptr && !cnx._endReported)) {
      ++pos;
    } else {
      removeConnection(pos);
    }
  }
}

}  // namespace aeronet
