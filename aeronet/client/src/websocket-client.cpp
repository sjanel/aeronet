#include "aeronet/websocket-client.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/adaptive-poll-timeout.hpp"
#include "aeronet/connection.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-view.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/log.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/upgraded-connection.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/websocket-client-handshake.hpp"
#include "aeronet/websocket-config.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-handler.hpp"

#ifdef AERONET_LINUX
#include "aeronet/sigpipe-blocker.hpp"
#endif

#ifdef AERONET_ENABLE_ZLIB
#include <array>

#include "aeronet/websocket-deflate.hpp"
#endif

namespace aeronet {

namespace {

// Reads of a connection per poll(): the other connections are served in between.
constexpr uint32_t kMaxReadsPerPoll = 4;

// The events watched for every connection: its socket is readable, or closed by the server.
constexpr EventBmp kReadEvents = EventIn | EventRdHup;

// Event slots of the event loop of a client: it serves a few connections.
constexpr uint32_t kNbInitialEvents = 16;

static_assert(http::HeaderSize(websocket::SecWebSocketKey.size(), websocket::WebSocketKey{}.size()) +
                  http::HeaderSize(websocket::SecWebSocketVersion.size(), websocket::kWebSocketVersion.size()) +
                  http::HeaderSize(websocket::SecWebSocketExtensions.size(), 128U) ==
              45U + 27U + 156U);

websocket::WebSocketConfig ClientSideConfig(websocket::WebSocketConfig config) {
  config.isServerSide = false;
  config.validate();
  return config;
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

// Makes room in 'vec' for one more element, growing it geometrically: the next emplace_back() cannot fail.
template <class Vector>
void ReserveOneMore(Vector& vec) {
  if (vec.size() == vec.capacity()) {
    vec.reserve(std::max<typename Vector::size_type>(2U * vec.size(), 4U));
  }
}

}  // namespace

WebSocketConnection::WebSocketConnection(PrivateTag /*tag*/, WebSocketClient& client, UpgradedConnection&& upgraded,
                                         const WebSocketClientCallbacks& callbacks, void* pUserData)
    : _handler(client._config),
      _transport(std::move(upgraded.transport)),
      _in(std::move(upgraded.received)),
      _callbacks(callbacks),
      _pUserData(pUserData),
      _pClient(&client),
      _lastActivity(SteadyClock::now()),
      _cnx(std::move(upgraded.cnx)),
      _interest(kReadEvents),
      _pendingInput(!_in.empty() || _transport.hasPendingReadData()),
      _unprocessedInput(!_in.empty()) {
  // The handler only knows its own callbacks: they forward to the ones of the connection, given with it. A message or
  // a Pong following the Close frame of the server in the same read is not delivered.
  websocket::WebSocketCallbacks handlerCallbacks{
      .onMessage = {},  // set below if the connection has one, like onPong
      .onPing = {},
      .onPong = {},
      .onClose = [this](websocket::CloseCode code,
                        std::string_view reason) { notifyClose(code, reason, State::Draining); },
      // The handler then sends its Close frame, and processInput() ends the connection.
      .onError = [this](websocket::CloseCode code,
                        std::string_view message) { notifyClose(code, message, State::Draining); },
  };
  if (callbacks.onMessage != nullptr) {
    handlerCallbacks.onMessage = [this](std::span<const std::byte> payload, bool isBinary) {
      if (_state == State::Open) {
        _callbacks.onMessage(*this, payload, isBinary);
      }
    };
  }
  if (callbacks.onPong != nullptr) {
    handlerCallbacks.onPong = [this](std::span<const std::byte> payload) {
      if (_state == State::Open) {
        _callbacks.onPong(*this, payload);
      }
    };
  }
  _handler.setCallbacks(std::move(handlerCallbacks));
}

bool WebSocketConnection::queued(bool ok) {
  // From a callback, poll() writes the frames once done with the received bytes. Otherwise they are written now, unless
  // the socket already refused bytes: poll() writes them all when it becomes writable.
  if (ok && !_pClient->_inPoll && !_writeBlocked && !flush()) {
    // The connection is lost: the next poll() ends it at once (onClose is not called from a send).
    _writeFailed = true;
    _pendingInput = true;
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
  while (_handler.pendingOutputSize() != 0) {
    // The buffered frames, then the payload sent from where it is (a large compressed message): a single write call.
    _handler.getPendingOutputFragments(fragments);
    const TransportResult res = _transport.write(std::span<const std::string_view>(fragments));
    _handler.onOutputWritten(res.bytesProcessed);
    if (res.want == TransportHint::Error) {
      return false;
    }
    if (res.want != TransportHint::None) {
      // The socket takes no more bytes for now (written when it is writable), or TLS needs to read first (the socket
      // readability is always watched, and each poll() writes the pending output again).
      _writeBlocked = res.want == TransportHint::WriteReady;
      break;
    }
  }
  return updateInterest();
}

bool WebSocketConnection::updateInterest() {
  return _pClient->watch(*this, _writeBlocked || _readWantsWrite ? kReadEvents | EventOut : kReadEvents);
}

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
  if (nextState == State::Draining) {
    // The server closes the TCP connection right after the closing handshake (RFC 6455 §7.1.1): it is closed after
    // closeTimeout otherwise.
    _drainDeadline = SteadyClock::now() + _handler.config().closeTimeout;
  }
  if (_callbacks.onClose != nullptr) {
    _callbacks.onClose(*this, code, reason);
  }
}

void WebSocketConnection::writePending(bool writable) {
  if (_state != State::Ended && _handler.pendingOutputSize() != 0 && (writable || !_writeBlocked) && !flush()) {
    lose("connection error");
  }
}

SteadyClock::time_point WebSocketConnection::nextDeadline() const noexcept {
  return _state == State::Draining ? _drainDeadline : _handler.nextTimeoutCheck(_lastActivity);
}

void WebSocketConnection::checkTimeouts(SteadyClock::time_point now) {
  if (_state == State::Draining) {
    if (now >= _drainDeadline) {
      _state = State::Ended;
    }
    return;
  }
  // A Ping queued for the idle connection is written with the pending output.
  switch (_handler.checkTimeouts(now, _lastActivity)) {
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

WebSocketClient::WebSocketClient(websocket::WebSocketConfig config, HttpClientConfig httpConfig)
    : _config(ClientSideConfig(std::move(config))),
      _http(std::move(httpConfig)),
      _loop(PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{1}}, kNbInitialEvents),
      _readChunkBytes(_http.config().minReadChunkBytes) {}

WebSocketClient::WebSocketClient(WebSocketClient&& rhs) noexcept
    : _config(std::move(rhs._config)),
      _http(std::move(rhs._http)),
      _loop(std::move(rhs._loop)),
      _connections(std::move(rhs._connections)),
      _fds(std::move(rhs._fds)),
      _fragments(std::move(rhs._fragments)),
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
    _http = std::move(rhs._http);
    _loop = std::move(rhs._loop);
    _fragments = std::move(rhs._fragments);
    _readChunkBytes = rhs._readChunkBytes;
    _pollTimeoutMs = rhs._pollTimeoutMs;
    for (const auto& pCnx : _connections) {
      pCnx->_pClient = this;
    }
  }
  return *this;
}

WebSocketClient::~WebSocketClient() = default;

WebSocketConnectResult WebSocketClient::connect(HttpRequest&& upgradeRequest, const WebSocketClientCallbacks& callbacks,
                                                void* pUserData) {
  assert(!_inPoll);  // the events being processed by poll() would refer to a moved buffer
  WebSocketConnectResult result;
  const websocket::WebSocketKey key = websocket::GenerateWebSocketKey();
  const std::string_view keyView(key.data(), key.size());
  upgradeRequest.headerAddLine(websocket::SecWebSocketKey, keyView);
  upgradeRequest.headerAddLine(websocket::SecWebSocketVersion, websocket::kWebSocketVersion);
#ifdef AERONET_ENABLE_ZLIB
  if (_config.deflateConfig.enabled) {
    std::array<char, websocket::kMaxDeflateOfferSize> offer;
    const char* offerEnd = websocket::BuildDeflateOffer(_config.deflateConfig, offer.data());
    upgradeRequest.headerAddLine(websocket::SecWebSocketExtensions, std::string_view(offer.data(), offerEnd));
  }
#endif

  HttpUpgradeResult upgraded = _http.upgrade(std::move(upgradeRequest));
  if (!upgraded) {
    result.httpError = upgraded.error();
    result.pErrorMessage = ErrcToStr(upgraded.error()).data();
    return result;
  }
  result.response = std::move(upgraded->response);
  if (!upgraded->connection.valid()) {
    result.pErrorMessage = "the server did not switch to the WebSocket protocol";
    return result;
  }
  const HttpResponse& response = result.response;
  // Each header is validated with all its lines (RFC 6455 §4.1 fails the connection on a second Sec-WebSocket-Protocol
  // or Sec-WebSocket-Extensions line of the response), copied only when it has several of them.
  // NOLINTNEXTLINE(bugprone-use-after-move) upgrade() leaves the request as sent: its offered subprotocols are there
  const HttpRequest& sentRequest = upgradeRequest;
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
      keyView, CombinedHeaderValue(sentRequest.headers(), websocket::SecWebSocketProtocol, offeredProtocolsLines),
      _config);
  if (!validation.valid()) {
    log::error("WebSocket client: invalid opening handshake response: {}", validation.pErrorMessage);
    result.pErrorMessage = validation.pErrorMessage;
    return result;  // the connection closes with the upgraded one
  }

  // Room in both vectors first: the connection is added to both (at the same index) or to none.
  ReserveOneMore(_connections);
  ReserveOneMore(_fds);
  WebSocketConnection* pCnx =
      _connections
          .emplace_back(std::make_unique<WebSocketConnection>(WebSocketConnection::PrivateTag{}, *this,
                                                              std::move(upgraded->connection), callbacks, pUserData))
          .get();
  _fds.push_back(pCnx->_cnx.fd());
#ifdef AERONET_ENABLE_ZLIB
  if (validation.deflateParams.hasValue()) {
    pCnx->_handler.enableCompression(validation.deflateParams);
  }
#endif
  if (!_loop.add(EventLoop::EventFd{pCnx->_cnx.fd(), kReadEvents})) {
    _fds.pop_back();
    _connections.pop_back();
    result.httpError = HttpClientErrc::ioError;
    result.pErrorMessage = ErrcToStr(HttpClientErrc::ioError).data();
    return result;
  }
  result.pConnection = pCnx;
  if (callbacks.onOpen != nullptr) {
    try {
      callbacks.onOpen(*pCnx);  // the frames it sends are written at once
    } catch (...) {
      // The caller never gets the connection: it ends at once.
      pCnx->_state = WebSocketConnection::State::Ended;
      removeEndedConnections();
      throw;
    }
  }
  return result;
}

std::size_t WebSocketClient::poll(std::chrono::milliseconds timeout) {
  assert(!_inPoll);
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
  bool pendingInput = false;
  for (const auto& pCnx : _connections) {
    pendingInput |= pCnx->_pendingInput;
    deadline = std::min(deadline, pCnx->nextDeadline());
  }

  std::span<const EventLoop::EventFd> events;
  if (pendingInput) {
    events = _loop.poll(false);  // bytes are already there: only collect the events ready
  } else {
    if (const int timeoutMs = PollTimeoutMs(now, deadline); timeoutMs != _pollTimeoutMs) {
      _loop.updatePollTimeoutPolicy(PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{timeoutMs}});
      _pollTimeoutMs = timeoutMs;
    }
    events = _loop.poll();
  }
  now = SteadyClock::now();

  WebSocketConnection* pServed = nullptr;  // the connection being served, ended if a callback throws
  try {
    for (const EventLoop::EventFd& event : events) {
      pServed = &find(event.fd);
      WebSocketConnection& cnx = *pServed;
      if ((event.eventBmp & EventOut) != 0) {
        cnx.writePending(true);
      }
      // Readable, closed or failed: the read tells, as it does once writable for a TLS read that had to write first.
      // kqueue (macOS) reports the readability and the writability of a socket as two events: an ended connection is
      // not read anymore.
      if (((event.eventBmp & ~EventOut) != 0 || cnx._readWantsWrite) &&
          cnx._state != WebSocketConnection::State::Ended) {
        cnx.read(now);
      }
    }

    // Index based: the callbacks may not add connections, but they run during the iteration.
    for (uint32_t pos = 0; pos < _connections.size(); ++pos) {
      pServed = _connections[pos].get();
      WebSocketConnection& cnx = *pServed;
      if (cnx._state != WebSocketConnection::State::Ended && cnx._pendingInput) {
        cnx.read(now);  // bytes held by TLS, or left by the read budget
      }
      if (cnx._state != WebSocketConnection::State::Ended) {
        cnx.checkTimeouts(now);
      }
      // The frames queued by the callbacks (messages, Pong, Close, Ping of an idle connection), written together.
      cnx.writePending(false);
    }
    // The callbacks of the last connections may have queued frames on the first ones.
    for (const auto& pCnx : _connections) {
      pServed = pCnx.get();
      pCnx->writePending(false);
    }
  } catch (...) {
    // A callback of the served connection threw (or one of its buffers could not grow): the bytes it processed are
    // unknown, so it ends at once, without onClose. The others carry on with the next poll().
    assert(pServed != nullptr);
    pServed->_state = WebSocketConnection::State::Ended;
    removeEndedConnections();
    throw;
  }

  removeEndedConnections();
  return _connections.size();
}

std::size_t WebSocketClient::nbConnections() const noexcept { return _connections.size(); }

WebSocketConnection& WebSocketClient::find(NativeHandle fd) const noexcept {
  const auto it = std::ranges::find(_fds, fd);
  // The sockets stay registered until their connection is removed, at the end of poll().
  assert(it != _fds.end());
  WebSocketConnection& cnx = *_connections[static_cast<uint32_t>(it - _fds.begin())];
  assert(cnx._cnx.fd() == fd);
  return cnx;
}

bool WebSocketClient::watch(WebSocketConnection& cnx, EventBmp interest) {
  if (cnx._interest == interest) {
    return true;
  }
  if (!_loop.mod(EventLoop::EventFd{cnx._cnx.fd(), interest})) {
    return false;
  }
  cnx._interest = interest;
  return true;
}

void WebSocketClient::removeEndedConnections() {
  for (uint32_t pos = 0; pos < _connections.size();) {
    if (_connections[pos]->_state == WebSocketConnection::State::Ended) {
      _loop.del(_fds[pos]);
      _fds[pos] = _fds.back();
      _fds.pop_back();
      std::swap(_connections[pos], _connections.back());
      _connections.pop_back();  // closes the connection
    } else {
      ++pos;
    }
  }
}

}  // namespace aeronet
