#include "upgrade-handshake.hpp"

#ifdef AERONET_WINDOWS
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/cookie-jar.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/upgraded-connection.hpp"
#include "http1-response-parser.hpp"
#include "proxy-tunnel.hpp"
#include "request-head-lines.hpp"
#include "transport-io.hpp"

#ifdef AERONET_LINUX
#include "aeronet/sigpipe-blocker.hpp"
#endif

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/http-client-tls-context.hpp"
#include "aeronet/temp-const-char-string.hpp"
#endif

namespace aeronet::internal {

namespace {

// Read size of the answer of a proxy, a few hundred bytes.
constexpr std::size_t kProxyReadChunk = 512;

}  // namespace

void UpgradeHandshake::CheckRequest(const HttpRequest& request) {
  if (request.hasBodyFile() || request.trailersSize() != 0) {
    throw std::invalid_argument("an upgrade request cannot have a file body nor trailers");
  }
}

void UpgradeHandshake::start(HttpClient& client, std::string_view extraLines) {
  reset();
  if (_request.isTlsRequest()) {
#ifdef AERONET_ENABLE_OPENSSL
    // Built (and validated) once, before spending a connect on it: an invalid configuration throws here.
    client.tlsContext();
#else
    throw std::logic_error("https requested but aeronet was built without OpenSSL support");
#endif
  }
  const HttpClientConfig& config = client.config();
  _response = HttpResponse();
  _parser.reset(false, false, true);  // a 101 ends the response, the bytes after its head belong to the new protocol
  _request.HttpMessage::finalizeHeadersAndBody();
  assert(!_request.hasBodyFile() && _request.trailersSize() == 0);  // see CheckRequest()

  // The CONNECT request first when the server is reached through a forward proxy: an upgraded connection carries
  // another protocol than HTTP, which a proxy would not relay, so a cleartext server is reached through a tunnel too
  // (RFC 6455 §4.1).
  if (client.usesProxy()) {
    BuildProxyConnectRequest(_request, _out);
    _proxyRequestSize = _out.size();
  }
  std::string_view cookies;
  if (config.cookies.enabled()) {
    client._cookieJar.prepareRequestCookies(_request, CookieSiteContext::SameSite, SysClock::now());
    cookies = client.requestCookies();
  }
  // An upgrade request has a Connection header: no "close" option.
  SpliceRequestHeadLines(_request.completeRequestForHttp11(), _request.headEndForHttp11(), cookies, extraLines, false,
                         _out);
  if (_request.hasBodyCaptured()) {
    _out.append(_request.capturedPayloadForHttp11());
  }
  _deadline = DeadlineAfter(SteadyClock::now(), config.connectTimeout);
  _phase = Phase::Resolve;
}

std::span<char> UpgradeHandshake::hostToResolve(const HttpClient& client) const noexcept {
  // Both hosts are followed by a byte of their buffer, that ResolveTCP() overwrites with a null terminator for a while.
  const std::string_view host = client.usesProxy() ? client._proxyHost : _request.host();
  return {const_cast<char*>(host.data()), host.size()};
}

uint16_t UpgradeHandshake::portToResolve(const HttpClient& client) const noexcept {
  return client.usesProxy() ? client._proxyPort : _request.port();
}

void UpgradeHandshake::onResolved(HttpClient& client, AddrInfoPtr addresses) {
  assert(_phase == Phase::Resolve);
  if (!addresses) {
    fail(HttpClientErrc::connectFailed);  // already logged by ResolveTCP()
    return;
  }
  _addresses = std::move(addresses);
  _pAddress = _addresses.get();
  // The connect has a budget of its own, as after a blocking resolution.
  _deadline = DeadlineAfter(SteadyClock::now(), client.config().connectTimeout);
  connect(client);
}

void UpgradeHandshake::connect(HttpClient& client) {
  ConnectResult result = StartConnectTCP(_pAddress);
  if (result.failure) {
    const std::span<char> host = hostToResolve(client);
    log::error("Upgrade handshake: unable to connect to {}:{}", std::string_view(host.data(), host.size()),
               portToResolve(client));
    fail(HttpClientErrc::connectFailed);
    return;
  }
  _cnx = std::move(result.cnx);
  const NativeHandle fd = _cnx.fd();
  SetNoSigPipe(fd);
  if (client.config().tcpNoDelay != TcpNoDelayMode::Disabled) {
    SetTcpNoDelay(fd);  // the request is sent without waiting for more data to coalesce
  }
  if (result.connectPending) {
    _phase = Phase::Connect;
    _interest = EventOut;  // writable once the connect completes, or failed
    return;
  }
  connected(client);
}

void UpgradeHandshake::connected(HttpClient& client) {
  // A tunnel through a proxy first, on the socket itself: no byte of the server comes before the client speaks.
  _transport = Transport(_cnx.fd());
  if (_proxyRequestSize != 0) {
    _deadline = DeadlineAfter(SteadyClock::now(), client.config().connectTimeout);
    _phase = Phase::ProxyWrite;
  } else {
    startSession(client);
  }
  proceed(client, false);
}

void UpgradeHandshake::startSession(HttpClient& client) {
#ifdef AERONET_ENABLE_OPENSSL
  if (_request.isTlsRequest()) {
    const HttpClientConfig& config = client.config();
    // Offering http/1.1 only through ALPN: a server preferring h2 would select it otherwise.
    _transport = client.tlsContext().makeTransport(_cnx.fd(), _request.hostCStr().c_str(), config.tlsVerifyPeer, true);
    _deadline = DeadlineAfter(SteadyClock::now(), config.connectTimeout);
    _phase = Phase::TlsHandshake;
    return;
  }
#endif
  startExchange(client);
}

void UpgradeHandshake::startExchange(const HttpClient& client) noexcept {
  _exchangeDeadline = DeadlineAfter(SteadyClock::now(), client.config().requestTimeout);
  _phase = Phase::WriteRequest;
  exchangeProgressed(client);
}

void UpgradeHandshake::advance(HttpClient& client, EventBmp events) {
  assert(inProgress() && _phase != Phase::Resolve && events != 0);
  if (_phase == Phase::Connect) {
    // The socket signaled the end of the connect: established, or failed (refused, unreachable...).
    if (const int err = GetSocketError(_cnx.fd()); err != 0) {
      log::debug("Upgrade handshake: connect to an address of {}:{} failed (err={}), trying the next one",
                 _request.host(), portToResolve(client), err);
      _cnx = Connection();  // the driver stops watching it (mayReplaceSocket())
      _pAddress = _pAddress->ai_next;
      connect(client);
      return;
    }
    connected(client);
    return;
  }
  proceed(client, (events & ~EventOut) != 0);
}

void UpgradeHandshake::proceed(HttpClient& client, bool readable) {
#ifdef AERONET_LINUX
  // OpenSSL writes on the socket itself, which raises SIGPIPE once the server reset the connection.
  const SigpipeBlocker sigpipeBlocker(_request.isTlsRequest());
#endif
  if (readable && _phase == Phase::WriteRequest) {
    // The server answered before reading the whole request (a refusal such as a 413 or a 431, which ends the attempt
    // without writing the rest), or closed the connection.
    readResponse(client);
    if (_phase != Phase::WriteRequest) {
      return;
    }
  }
  for (;;) {
    switch (_phase) {
      case Phase::ProxyWrite:
        if (!writeOut(client, _proxyRequestSize, HttpClientErrc::proxyError)) {
          return;
        }
        _phase = Phase::ProxyRead;
        break;
      case Phase::ProxyRead:
        if (!readProxyResponse()) {
          return;
        }
        startSession(client);
        break;
      case Phase::TlsHandshake: {
        const TransportResult res = _transport.write(std::string_view{});
        if (_transport.handshakeDone()) {
          startExchange(client);
          break;
        }
        if (res.want == TransportHint::Error) {
          fail(HttpClientErrc::tlsError);
          return;
        }
        _interest = InterestFor(res.want);
        return;
      }
      case Phase::WriteRequest:
        if (!writeOut(client, _out.size(), HttpClientErrc::writeError)) {
          if (_phase == Phase::WriteRequest) {
            _interest |= EventIn;  // the response may come before the server read all of the request
          }
          return;
        }
        _phase = Phase::ReadResponse;
        _interest = EventIn;
        // The response takes a round trip at least: waiting before the first read saves a read failing with EAGAIN.
        // The bytes TLS already holds are not signaled by the socket.
        if (!_transport.hasPendingReadData()) {
          return;
        }
        break;
      default:
        assert(_phase == Phase::ReadResponse);  // advance() serves the connect, start() the resolution
        readResponse(client);
        return;
    }
  }
}

bool UpgradeHandshake::writeOut(HttpClient& client, std::size_t end, HttpClientErrc error) {
  while (_written < end) {
    const TransportResult res = _transport.write(std::string_view(_out.data() + _written, end - _written));
    _written += res.bytesProcessed;
    if (res.bytesProcessed != 0 && _phase == Phase::WriteRequest) {
      exchangeProgressed(client);
    }
    if (_written == end) {
      break;
    }
    if (res.want == TransportHint::Error) {
      fail(error);
      return false;
    }
    if (res.want != TransportHint::None) {
      _interest = InterestFor(res.want);
      return false;
    }
  }
  return true;
}

bool UpgradeHandshake::readProxyResponse() {
  for (;;) {
    switch (CheckProxyTunnelResponse(_in)) {
      case ProxyTunnelStatus::Open:
        _in.clear();
        return true;
      case ProxyTunnelStatus::Refused:
        fail(HttpClientErrc::proxyError);
        return false;
      default:
        break;
    }
    const TransportResult res = ReadAppend(_transport, _in, kProxyReadChunk);
    if (res.bytesProcessed != 0) {
      continue;
    }
    if (res.want != TransportHint::ReadReady && res.want != TransportHint::WriteReady) {
      fail(HttpClientErrc::proxyError);  // the proxy closed the connection before answering
      return false;
    }
    _interest = InterestFor(res.want);
    return false;
  }
}

void UpgradeHandshake::readResponse(HttpClient& client) {
  const HttpClientConfig& config = client.config();
  for (;;) {
    const TransportResult res = ReadAppend(_transport, _in, config.minReadChunkBytes);
    bool eof = false;
    if (res.bytesProcessed == 0) {
      if (res.want == TransportHint::ReadReady || res.want == TransportHint::WriteReady) {
        // TLS may have to write before it reads again.
        _interest = InterestFor(res.want);
        return;
      }
      eof = true;  // closed by the server, or failed: the parser tells whether the response is complete
    } else {
      exchangeProgressed(client);
    }
    // Set for each parse: the client may have moved since the previous one. A body is decoded whole, when complete.
    if (config.decompression.enable) {
      _parser.setDecodeContext({
          .pState = &client._codec.decompressionState,
          .pConfig = &config.decompression,
          .pOut = &client._codec.decompressOut,
          .pTmp = &client._codec.decompressTmp,
      });
    }
    switch (_parser.parse(_in, eof, _response, config.maxResponseBytes)) {
      case Http1ResponseParser::Status::NeedMore:
        assert(!eof);  // once eof is set, the parser always resolves to a final status
        continue;
      case Http1ResponseParser::Status::Complete:
        break;
      case Http1ResponseParser::Status::Truncated:
        log::error("Upgrade handshake: {} closed the connection before the end of the response", _request.originKey());
        fail(HttpClientErrc::connectionClosed);
        return;
      case Http1ResponseParser::Status::TooLarge:
        log::error("Upgrade handshake: response of {} exceeds maxResponseBytes ({}) or the decompression limits",
                   _request.originKey(), config.maxResponseBytes);
        fail(HttpClientErrc::responseTooLarge);
        return;
      default:
        // HeadComplete and BodyPiece are only returned to a streaming caller.
        log::error("Upgrade handshake: malformed response from {}", _request.originKey());
        fail(HttpClientErrc::malformedResponse);
        return;
    }
    if (config.cookies.enabled()) {
      // A refusal sets its cookies too.
      client._cookieJar.storeResponseCookies(_request, _response, CookieSiteContext::SameSite, SysClock::now());
    }
    if (_response.status() == http::StatusCodeSwitchingProtocols) {
      if (_phase == Phase::WriteRequest) {
        // The rest of the request would be taken for bytes of the new protocol.
        log::error("Upgrade handshake: {} switched protocols before the end of the request", _request.originKey());
        fail(HttpClientErrc::malformedResponse);
        return;
      }
      // The bytes following the head of the 101 are the first ones of the new protocol (RFC 9110 §15.2.2), and TLS may
      // hold more of them, already decrypted.
      _in.erase_front(_parser.consumed());
    }
    _phase = Phase::Done;
    _interest = 0;
    return;
  }
}

void UpgradeHandshake::exchangeProgressed(const HttpClient& client) noexcept {
  _deadline = std::min(_exchangeDeadline, DeadlineAfter(SteadyClock::now(), client.config().inactivityTimeout));
}

void UpgradeHandshake::fail(HttpClientErrc error) noexcept {
  assert(error != HttpClientErrc::noError);
  _error = error;
  _phase = Phase::Failed;
  _interest = 0;
}

void UpgradeHandshake::run(HttpClient& client, std::string_view extraLines) {
  // The socket last waited for in the event loop of the client leaves it once replaced, and on every exit (an exception
  // included), even when closed already: the loop would otherwise take a new socket reusing its number for the
  // registered one, and WSAPoll (Windows) would keep polling it.
  class LoopWatch {
   public:
    explicit LoopWatch(HttpClient& client) noexcept : _client(client) {}

    LoopWatch(const LoopWatch&) = delete;
    LoopWatch(LoopWatch&&) = delete;
    LoopWatch& operator=(const LoopWatch&) = delete;
    LoopWatch& operator=(LoopWatch&&) = delete;

    ~LoopWatch() { stop(); }

    EventBmp wait(NativeHandle fd, EventBmp interest, SteadyClock::time_point deadline) {
      _fd = fd;
      return _client.waitEvents(fd, interest, deadline);
    }

    void stop() noexcept {
      if (_fd != kInvalidHandle) {
        _client.unregisterIfCurrent(_fd);
        _fd = kInvalidHandle;
      }
    }

   private:
    HttpClient& _client;
    NativeHandle _fd{kInvalidHandle};
  };

  start(client, extraLines);
  onResolved(client, ResolveTCP(hostToResolve(client), portToResolve(client)));
  LoopWatch loopWatch(client);
  while (inProgress()) {
    if (mayReplaceSocket()) {
      loopWatch.stop();  // a failed connect closed the socket waited for, replaced by the connect to the next address
    }
    const EventBmp events = loopWatch.wait(fd(), _interest, _deadline);
    if (events == 0) {
      expire();  // or a failure of the event loop, already logged
      break;
    }
    advance(client, events);
  }
}

void UpgradeHandshake::reset() noexcept {
  if (_phase == Phase::Idle) {
    return;
  }
  {
    // Left moved from until the next start(): only read in the Done phase.
    [[maybe_unused]] const HttpResponse released(std::move(_response));
  }
  _out = RawChars();
  _in = RawChars();
  _bodyBuffer = RawChars();
  _addresses.reset();
  _pAddress = nullptr;
  _transport = Transport();
  _proxyRequestSize = 0;
  _written = 0;
  _cnx = Connection();
  _interest = 0;
  _error = HttpClientErrc::noError;
  _phase = Phase::Idle;
}

UpgradedConnection UpgradeHandshake::takeConnection() noexcept {
  assert(_phase == Phase::Done && _response.status() == http::StatusCodeSwitchingProtocols);
  UpgradedConnection upgraded;
  upgraded.transport = std::move(_transport);
  upgraded.received = std::move(_in);  // the receive buffer goes with them
  upgraded.cnx = std::move(_cnx);
  return upgraded;
}

}  // namespace aeronet::internal
