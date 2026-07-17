#include "aeronet/http-client.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "aeronet/adaptive-poll-timeout.hpp"
#include "aeronet/client-connection.hpp"
#include "aeronet/client-protocol.hpp"
#include "aeronet/connection.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client-exception.hpp"
#include "aeronet/http-codec-result.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/url-parsed-result.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/retry-config.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/zerocopy-mode.hpp"
#include "client-accept-encoding.hpp"
#include "url-parse.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/ssl.h>

#include "aeronet/http-client-tls-context.hpp"
#include "aeronet/tls-transport.hpp"
#endif

#if !defined(AERONET_ENABLE_ZLIB) || !defined(AERONET_ENABLE_ZSTD) || !defined(AERONET_ENABLE_BROTLI)
#include "aeronet/encoding.hpp"
#endif

namespace aeronet {

namespace {

// Fixed, short poll interval so the synchronous driver re-checks its deadline promptly.
constexpr EventLoop CreateEventLoop() {
  PollTimeoutPolicy policy;
  policy.baseTimeout = std::chrono::milliseconds{25};
  policy.minFactor = 1.0F;
  policy.maxFactor = 1.0F;
  return EventLoop{std::move(policy)};
}

constexpr bool IsRedirect(http::StatusCode code) noexcept {
  return code == http::StatusCodeMovedPermanently || code == http::StatusCodeFound ||
         code == http::StatusCodeSeeOther || code == http::StatusCodeTemporaryRedirect ||
         code == http::StatusCodePermanentRedirect;
}

// Parse a delta-seconds `Retry-After` header value into a non-negative duration capped at `cap`. Returns a
// negative sentinel when the value is absent or not a bare non-negative integer (an HTTP-date form is
// intentionally not parsed here) so the caller can distinguish it from a legitimate "Retry-After: 0" and
// fall back to the computed backoff.
RetryConfig::Duration ParseRetryAfter(std::string_view value, RetryConfig::Duration cap) noexcept {
  uint64_t seconds = 0;
  const char* end = value.data() + value.size();
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  const auto [ptr, ec] = std::from_chars(value.data(), end, seconds);
  if (ec != std::errc{} || ptr != end) {
    return RetryConfig::Duration{-1};  // absent / not delta-seconds: caller uses the computed backoff
  }
  const auto capSeconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(cap).count());
  return std::chrono::duration_cast<RetryConfig::Duration>(std::chrono::seconds{std::min(seconds, capSeconds)});
}

// Interpret the status line of a proxy CONNECT response ("HTTP/1.x <code> <reason>"). A 2xx code means the
// tunnel is open; anything else (or a line we cannot parse a status code out of) is a proxy failure.
std::expected<void, HttpClientErrc> CheckProxyTunnelStatus(std::string_view statusLine) {
  const auto sp = statusLine.find(' ');
  if (sp == std::string_view::npos) {
    return std::unexpected(HttpClientErrc::proxyError);
  }
  const std::string_view rest = statusLine.substr(sp + 1);
  uint32_t code = 0;
  const auto [ptr, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), code);
  if (ec != std::errc{} || code < 200 || code >= 300) {
    return std::unexpected(HttpClientErrc::proxyError);
  }
  return {};
}

}  // namespace

HttpClient::HttpClient(HttpClientConfig config)
    : _config(std::move(config)),
      _loop(CreateEventLoop()),
      _codec(_config.requestCompression.codec),
      _telemetry(_config.telemetry) {
  // Fail fast on inconsistent configuration (mirrors the server's validate-on-construct policy): timeouts,
  // HTTP version selection (Http2 requires a build with AERONET_ENABLE_HTTP2) and HTTP/2 settings.
  _config.validate();

  // Bake the resolved default Accept-Encoding into the per-request global headers so every request the
  // client builds advertises it (a per-request Accept-Encoding header still overrides it, since header()
  // replaces the first occurrence). An explicit defaultAcceptEncoding() wins; otherwise, when response
  // decompression is enabled, advertise exactly what this build can decode so origins know they may compress
  // (and the response is transparently decoded). Nothing is injected when neither applies.
  {
    std::string_view acceptEncoding = _config.defaultAcceptEncoding();
    if (acceptEncoding.empty() && _config.decompression.enable) {
      acceptEncoding = internal::kSupportedAcceptEncoding;
    }
    if (!acceptEncoding.empty() && std::ranges::none_of(_config.globalHeaders, [](std::string_view part) {
          const auto colon = part.find(':');
          return colon != std::string_view::npos && CaseInsensitiveEqual(part.substr(0, colon), http::AcceptEncoding);
        })) {
      RawChars line(http::AcceptEncoding.size() + http::HeaderSep.size() + acceptEncoding.size());
      line.unchecked_append(http::AcceptEncoding);
      line.unchecked_append(http::HeaderSep);
      line.unchecked_append(acceptEncoding);
      _config.globalHeaders.append(line);
    }
  }
  // throws here deterministically rather than failing every request. Only cleartext (http) proxies are
  // supported. _proxyHost keeps a spare trailing byte for ConnectTCP's transient null-termination.
  if (_config.hasProxy()) {
    std::string_view proxyUrl = _config.proxyUrl();
    const auto schemeEnd = proxyUrl.find("://");
    std::string_view scheme = (schemeEnd == std::string_view::npos) ? "http" : proxyUrl.substr(0, schemeEnd);
    if (!CaseInsensitiveEqual(scheme, "http")) {
      throw HttpClientException("Only cleartext (http) forward proxies are supported");
    }
    if (schemeEnd != std::string_view::npos) {
      proxyUrl = proxyUrl.substr(schemeEnd + 3);
    }

    internal::UrlParseResult res;
    internal::ParseAuthority(proxyUrl, res);

    _proxyHost = res.host;
    _proxyPort = res.port;
  }
}

HttpClientResult HttpClient::requestProcess(HttpRequest&& req) {
  const bool isCacheEligible = cacheEligible(req);

  maybeCompressRequestBody(req);

  std::string_view cacheKey;
  if (isCacheEligible) {
    cacheKey = buildCacheKey(req);
    HttpResponse* pCachedHttpResponse = cacheLookupFresh(cacheKey);
    if (pCachedHttpResponse != nullptr) {
      return pCachedHttpResponse->cloneFinalized();
    }
  }
  HttpClientResult result = requestUncached(std::move(req));
  // Cache only genuine 2xx responses; transport errors and non-success statuses are never stored.
  if (isCacheEligible && result && result->status() >= 200 && result->status() < 300) {
    cacheStore(cacheKey, *result);
  }

  return result;
}

bool HttpClient::armLoop(NativeHandle fd, EventBmp interest) {
  if (_loopFd == fd) {
    // Same connection as the last wait: reuse the registration, re-arming only if the interest changed
    // (the common response-read path keeps EventIn, so this is a no-op -- no syscall).
    if (_loopInterest == interest) {
      return true;
    }
    if (!_loop.mod(EventLoop::EventFd{fd, interest})) {
      return false;
    }
    _loopInterest = interest;
    return true;
  }
  // Switching to a different fd: drop the previous registration (a connection was pooled or closed) and
  // register this one. Only ever one fd is watched at a time, so the poll loop never sees a foreign fd.
  if (_loopFd != kInvalidHandle) {
    _loop.del(_loopFd);
  }
  if (!_loop.add(EventLoop::EventFd{fd, interest})) {
    _loopFd = kInvalidHandle;
    _loopInterest = 0;
    return false;
  }
  _loopFd = fd;
  _loopInterest = interest;
  return true;
}

void HttpClient::unregisterIfCurrent(NativeHandle fd) noexcept {
  if (fd == _loopFd) {
    _loop.del(_loopFd);
    _loopFd = kInvalidHandle;
    _loopInterest = 0;
  }
}

void HttpClient::dropConnection(ActiveConnection& conn) noexcept {
  // Only ever called on a live connection: freshly connected, or just popped from the idle pool.
  assert(static_cast<bool>(conn.cnx));
  unregisterIfCurrent(conn.cnx.fd());
  conn.reset();
}

void HttpClient::dropIdleBucket(vector<ActiveConnection>& bucket) noexcept {
  for (ActiveConnection& conn : bucket) {
    // Pooled entries are always live: releaseConnection never stores an invalid connection.
    assert(static_cast<bool>(conn.cnx));
    unregisterIfCurrent(conn.cnx.fd());
  }
  bucket.clear();
}

void HttpClient::clearIdleConnections() {
  for (auto& [key, bucket] : _idle) {
    dropIdleBucket(bucket);
  }
  _idle.clear();
}

bool HttpClient::waitIo(NativeHandle fd, EventBmp interest, SteadyClock::time_point deadline) {
  if (!armLoop(fd, interest)) {
    return false;
  }
  for (;;) {
    if (SteadyClock::now() >= deadline) {
      return false;
    }
    const std::span<const EventLoop::EventFd> events = _loop.poll();
    if (events.data() == nullptr) {
      return false;  // unrecoverable poll failure (already logged)
    }
    for (const auto& ev : events) {
      if (ev.fd == fd) {
        // Surface error/hup to the next I/O attempt, and report interest readiness.
        if ((ev.eventBmp & (interest | EventErr | EventHup | EventRdHup)) != 0U) {
          return true;
        }
      }
    }
    // Empty span => poll timeout; loop and re-check the deadline.
  }
}

std::expected<HttpClient::ActiveConnection, HttpClientErrc> HttpClient::connectNew(const HttpRequest& req) {
  // For https, build (and thereby validate) the shared TLS context up front -- before spending a connect on
  // it. A misconfiguration (bad cipher list, unusable cert/key, ...) is a hard setup error: it throws an
  // HttpClientException deterministically here, independently of whether the origin is even reachable.
  // The context is built once (lazily, on the first https request) and reused; this is a no-op on every
  // subsequent connect.
  const bool isTls = req.isTlsRequest();
  if (isTls) {
#ifdef AERONET_ENABLE_OPENSSL
    tlsContext();
#else
    throw std::logic_error("https requested but aeronet was built without OpenSSL support");
#endif
  }

  // With a forward proxy configured, connect to the proxy instead of the origin (the origin is reached
  // through it -- via CONNECT for https, absolute-form requests for http). Otherwise connect straight to the
  // origin. Both host spans keep a spare writable byte for ConnectTCP's transient null-termination.
  const bool proxied = usesProxy();
  const auto hostWithoutPort = req.host();

  const std::span<char> connectHost =
      proxied ? std::span<char>(const_cast<char*>(_proxyHost.data()), _proxyHost.size())
              : std::span<char>(const_cast<char*>(hostWithoutPort.data()), hostWithoutPort.size());
  const uint16_t connectPort = proxied ? _proxyPort : req.port();

  // getaddrinfo (via ConnectTCP) needs a writable host buffer with one spare byte at the end (done at
  // construction). Opt into ConnectTCP's blocking multi-address fallback (bounded by the connect timeout): a host
  // that resolves to several addresses (e.g. "localhost" -> ::1 then 127.0.0.1) must try them in turn instead of
  // committing to the first, whose non-blocking connect would otherwise hide a deferred ECONNREFUSED.
  const auto connectTimeoutMs = static_cast<int>(_config.connectTimeout.count());
  ConnectResult cr = ConnectTCP(connectHost, connectPort, 0, connectTimeoutMs);
  if (cr.failure || !cr.cnx) {
    return std::unexpected(HttpClientErrc::connectFailed);
  }
  ActiveConnection conn;
  conn.cnx = std::move(cr.cnx);
  const NativeHandle fd = conn.cnx.fd();
  SetNoSigPipe(fd);
  // Disable Nagle unless explicitly turned off: a request/response client wants the request flushed
  // without waiting for more data to coalesce (Auto resolves to enabled here).
  if (_config.tcpNoDelay != TcpNoDelayMode::Disabled) {
    SetTcpNoDelay(fd);
  }
  // For an https origin reached through a proxy, open the CONNECT tunnel to the origin now -- before the
  // (origin) TLS handshake driven later by finishConnect. The tunnel exchange runs on the raw socket via a
  // throwaway plain transport (no origin bytes can arrive until we speak TLS, so nothing beyond the CONNECT
  // response is consumed); the TLS transport then wraps the same fd and handshakes through the tunnel.
  if (proxied && isTls) {
    PlainTransport tunnelTransport(fd, ZerocopyMode::Disabled, ~0U);
    if (auto tunnel = establishProxyTunnel(tunnelTransport, fd, req); !tunnel) {
      unregisterIfCurrent(fd);  // establishProxyTunnel may have armed the loop on this fd before failing
      return std::unexpected(tunnel.error());
    }
  }
#ifdef AERONET_ENABLE_OPENSSL
  if (isTls) {
    // The context is already built above; makeTransport only wraps the freshly connected fd.
    conn.transport = tlsContext().makeTransport(fd, req.hostCStr().c_str(), _config.tlsVerifyPeer);
  } else
#endif
  {
    conn.transport = std::make_unique<PlainTransport>(fd, ZerocopyMode::Disabled, ~0U);
  }
  return conn;
}

std::expected<void, HttpClientErrc> HttpClient::establishProxyTunnel(ITransport& transport, NativeHandle fd,
                                                                     const HttpRequest& req) {
  static constexpr std::string_view kConnect = "CONNECT ";
  static constexpr std::string_view kConnectMid = " HTTP/1.1\r\nHost: ";  // between request-target and Host value

  const auto deadline = SteadyClock::now() + _config.connectTimeout;

  // CONNECT needs an explicit "host:port" authority (RFC 9110 section 9.3.6); IPv6 literals are bracketed.
  const std::string_view host = req.host();
  const bool ipv6 = host.contains(':');
  const uint16_t port = req.port();
  const auto portDigits = ndigits(port);
  const std::size_t authorityLen = host.size() + (ipv6 ? 2U : 0U) + 1U + portDigits;

  RawChars& reqBuffer = _reqBodyScratch;
  reqBuffer.reserve(kConnect.size() + authorityLen + kConnectMid.size() + authorityLen + http::DoubleCRLF.size());
  const auto appendAuthority = [&](char* out) {
    if (ipv6) {
      *out++ = '[';
    }
    out = Append(host, out);
    if (ipv6) {
      *out++ = ']';
    }
    *out++ = ':';
    return std::to_chars(out, out + portDigits, port).ptr;
  };
  char* pEnd = reqBuffer.data();
  pEnd = Append(kConnect, pEnd);
  pEnd = appendAuthority(pEnd);
  pEnd = Append(kConnectMid, pEnd);
  pEnd = appendAuthority(pEnd);
  pEnd = Append(http::DoubleCRLF, pEnd);
  reqBuffer.setSize(static_cast<std::size_t>(pEnd - reqBuffer.data()));

  // Write the CONNECT request in full, pumping the event loop on would-block.
  const std::string_view head = reqBuffer;
  std::size_t off = 0;
  while (off < head.size()) {
    const ITransport::TransportResult wr = transport.write(head.substr(off));
    off += wr.bytesProcessed;
    if (off >= head.size()) {
      break;
    }
    if (wr.want == TransportHint::Error) {
      return std::unexpected(HttpClientErrc::proxyError);
    }
    const EventBmp interest = (wr.want == TransportHint::ReadReady) ? EventIn : EventOut;
    if (!waitIo(fd, interest, deadline)) {
      return std::unexpected(HttpClientErrc::timeout);
    }
  }

  // Read the proxy response up to the end-of-headers marker. A CONNECT response is small; cap the buffered
  // size so a misbehaving proxy cannot make us read unbounded data.
  static constexpr std::size_t kMaxTunnelResponse = 8192;
  static constexpr std::size_t kReadChunk = 512;
  RawChars& respBuffer = _responseBuffer;
  respBuffer.clear();
  for (;;) {
    const std::string_view view(respBuffer.data(), respBuffer.size());
    if (const auto endPos = view.find(http::DoubleCRLF); endPos != std::string_view::npos) {
      // Status line is everything up to the first CRLF (guaranteed present within [0, endPos]).
      return CheckProxyTunnelStatus(view.substr(0, view.find(http::CRLF)));
    }
    if (respBuffer.size() >= kMaxTunnelResponse) {
      return std::unexpected(HttpClientErrc::proxyError);
    }
    respBuffer.ensureAvailableCapacityExponential(kReadChunk);
    const ITransport::TransportResult rd = transport.read(respBuffer.data() + respBuffer.size(), kReadChunk);
    if (rd.bytesProcessed > 0) {
      respBuffer.addSize(rd.bytesProcessed);
      continue;
    }
    if (rd.want == TransportHint::ReadReady) {
      if (!waitIo(fd, EventIn, deadline)) {
        return std::unexpected(HttpClientErrc::timeout);
      }
      continue;
    }
    if (rd.want == TransportHint::WriteReady) {
      if (!waitIo(fd, EventOut, deadline)) {
        return std::unexpected(HttpClientErrc::timeout);
      }
      continue;
    }
    // 0 bytes and no want => the proxy closed before completing the tunnel handshake.
    return std::unexpected(HttpClientErrc::proxyError);
  }
}

std::expected<HttpClient::ActiveConnection, HttpClientErrc> HttpClient::acquireConnection(const HttpRequest& req) {
  if (_config.keepAlive) {
    if (auto it = _idle.find(req.originKey()); it != _idle.end() && !it->second.empty()) {
      auto& bucket = it->second;
      // Bucket is LIFO: back() is the most-recently-released (freshest) connection. If even it has
      // exceeded the idle limit, every older entry has too, so drop the whole bucket and reconnect.
      if (_config.keepAliveTimeout.count() > 0 &&
          SteadyClock::now() - bucket.back().idleSince > _config.keepAliveTimeout) {
        dropIdleBucket(bucket);
      } else {
        ActiveConnection conn = std::move(bucket.back());
        bucket.pop_back();
        // Vet the pooled connection before reusing it. A keep-alive connection the origin already closed
        // (its own idle timeout) would otherwise accept the request bytes locally and fail only on the
        // subsequent read, where a transparent retry could re-submit a non-idempotent request. Discard a
        // stale connection here (and its likely-stale siblings) so the request is always issued on a socket
        // we have just confirmed is still open. IsConnectionStale also treats *pending bytes* as stale,
        // which for HTTP/2 covers a GOAWAY (or any housekeeping frame) that arrived while idle: the
        // connection is simply reconnected rather than reused. The engine is additionally consulted so a
        // multiplexing protocol that cannot host one more stream is never handed out.
        if (!IsConnectionStale(conn.cnx.fd()) && conn.proto.canTakeAnotherStream()) {
          conn.reused = true;
          return conn;
        }
        dropConnection(conn);    // unregister + close the stale connection we popped
        dropIdleBucket(bucket);  // and its likely-stale siblings
      }
    }
  }
  return connectNew(req);
}

void HttpClient::releaseConnection(const HttpRequest& req, ActiveConnection&& conn) {
  assert(_config.keepAlive);
  // Only released after a completed exchange, so the connection is always live at this point.
  assert(static_cast<bool>(conn.cnx));
  // This is the only place the pool needs to own (and therefore allocate) an origin key: create the
  // bucket on first release for this origin so subsequent requests can actually reuse the connection.
  // The key is materialized once per distinct origin and never rebuilt; all other accesses are
  // transparent string_view lookups.
  auto& bucket = _idle.try_emplace(req.originKey()).first->second;
  if (bucket.size() >= _config.maxIdleConnectionsPerHost) {
    dropConnection(conn);  // pool full: unregister its fd from the loop before it closes
    return;
  }
  conn.idleSince = SteadyClock::now();  // stamp for idle-expiry on the next acquire
  bucket.emplace_back(std::move(conn));
}

std::expected<void, HttpClientErrc> HttpClient::finishConnect(ActiveConnection& conn, bool isTls,
                                                              SteadyClock::time_point deadline) {
  const NativeHandle fd = conn.cnx.fd();
  // The TCP connect (including multi-address fallback) is already complete here: connectNew() resolves it
  // synchronously via ConnectTCP's blocking fallback. Only the TLS handshake remains.
  // Drive the TLS handshake to completion (no-op for plain transport).
  while (!conn.transport->handshakeDone()) {
    const ITransport::TransportResult transportRes = conn.transport->write(std::string_view{});
    if (conn.transport->handshakeDone()) {
      break;
    }
    if (transportRes.want == TransportHint::Error) {
      return std::unexpected(HttpClientErrc::tlsError);
    }
    const EventBmp interest = (transportRes.want == TransportHint::ReadReady) ? EventIn : EventOut;
    if (!waitIo(fd, interest, deadline)) {
      return std::unexpected(HttpClientErrc::timeout);
    }
  }
  // This is the single point where conn.protocol is decided for a fresh connection; a reused pooled
  // connection keeps the protocol it negotiated originally (re-resolving here is idempotent).
#ifdef AERONET_ENABLE_OPENSSL
  // Resolve the negotiated application protocol from ALPN. Only https negotiates ALPN; an empty / unknown
  // selection stays HTTP/1.1 -- unless HTTP/2 is required, in which case the connection is unusable
  // (over TLS, HTTP/2 requires an explicit ALPN "h2" selection, RFC 9113 §3.3).
  if (isTls && conn.transport) {
    const unsigned char* alpn = nullptr;
    unsigned int alpnLen = 0;
    ::SSL_get0_alpn_selected(static_cast<TlsTransport*>(conn.transport.get())->rawSsl(), &alpn, &alpnLen);
    if (alpn != nullptr && alpnLen != 0) {
      conn.protocol = ClientProtocolFromAlpnId(std::string_view(reinterpret_cast<const char*>(alpn), alpnLen));
    }
    if (_config.httpVersion == HttpVersionMode::Http2 && conn.protocol != ClientProtocol::Http2) {
      log::error("HTTP/2 required but the origin did not select ALPN \"h2\"");
      return std::unexpected(HttpClientErrc::protocolUnsupported);
    }
  }
#endif
  // Plain http with HTTP/2 required: speak h2c with prior knowledge (RFC 9113 §3.4) -- the client sends
  // its connection preface directly. Auto stays HTTP/1.1 on cleartext (the Upgrade dance is deprecated).
  if (!isTls && _config.httpVersion == HttpVersionMode::Http2) {
    conn.protocol = ClientProtocol::Http2;
  }
  return {};
}

std::expected<void, HttpClientErrc> HttpClient::ensureProtocolHandler(ActiveConnection& conn) const {
  if (!conn.proto.empty()) {
    return {};  // reused from the pool: the protocol engine (and any per-connection state) travels with it
  }
  switch (conn.protocol) {
    case ClientProtocol::Http2:
#ifdef AERONET_ENABLE_HTTP2
      conn.proto = internal::ClientConnection(_config.http2);
      return {};
#else
      // Unreachable in practice: without HTTP/2 support "h2" is never advertised (so ALPN cannot select
      // it) and HttpVersionMode::Http2 is rejected at construction. Kept as a defensive guard.
      return std::unexpected(HttpClientErrc::protocolUnsupported);
#endif
    case ClientProtocol::Http1_1:
      break;
  }
  conn.proto = internal::ClientConnection(internal::ClientConnection::Type::Http11);
  return {};
}

#ifdef AERONET_ENABLE_OPENSSL
internal::HttpClientTlsContext& HttpClient::tlsContext() {
  if (_tls.empty()) {
    // Build (and validate) the SSL_CTX once, on the first https request. A bad TLS configuration throws
    // HttpClientException here rather than at construction, so a plain-http client never pays for it.
    _tls = internal::HttpClientTlsContext(_config);
  }
  return _tls;
}
#endif

double HttpClient::nextJitterUnit() noexcept {
  // xorshift64* step; map the top 53 bits to a double in [0, 1). Quality is irrelevant here -- this only
  // spreads backoff delays so a fleet of clients does not retry in lockstep.
  _jitterState ^= _jitterState << 13;
  _jitterState ^= _jitterState >> 7;
  _jitterState ^= _jitterState << 17;
  return static_cast<double>(_jitterState >> 11) * (1.0 / 9007199254740992.0);  // 1 / 2^53
}

HttpRequest::Options HttpClient::makeRequestOptions() noexcept {
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
  HttpRequest::Options opts(_codec.compressionState, _config.requestCompression.encoding);
  // Vary header does not make sense for a request, we don't add it.
#else
  HttpRequest::Options opts;
#endif
  if (!_config.keepAlive) {
    opts.setClose();
  }
  if (_config.addTrailerHeader) {
    opts.addTrailerHeader();
  }
  if (_config.hasProxy()) {
    opts.setHasProxy();
  }
  opts.setHttpRequest();
  opts.setPrepared();
  return opts;
}

HttpClientResult HttpClient::performExchange(const HttpRequest& req) {
  const RetryConfig& retry = _config.retry;
  const uint32_t maxAttempts = retry.maxAttempts < 1U ? 1U : retry.maxAttempts;
  uint32_t attempt = 0;  // backoff retries already consumed (0-based index of the next one)

  // Whether the backoff budget still allows one more retry.
  const auto canBackoff = [&] { return attempt + 1U < maxAttempts; };
  // Sleep `delay` (a zero delay -- e.g. "Retry-After: 0" -- means retry immediately) and consume one slot.
  const auto sleepBackoff = [&](RetryConfig::Duration delay) {
    if (delay > RetryConfig::Duration::zero()) {
      std::this_thread::sleep_for(delay);
    }
    _telemetry.counterAdd("aeronet.http_requests.retries", 1);
    ++attempt;
  };
  // Computed exponential-backoff delay for the upcoming retry (jitter consulted only when enabled).
  const auto computedBackoff = [&] { return retry.delayFor(attempt, retry.jitter > 0.0F ? nextJitterUnit() : 0.0); };

  for (;;) {
    std::expected<ActiveConnection, HttpClientErrc> acquired = acquireConnection(req);
    if (!acquired) {
      // A fresh connect failed (acquireConnection already drained any stale pool entry and reconnected).
      // Nothing was sent, so a backoff retry is always safe.
      if (canBackoff()) {
        sleepBackoff(computedBackoff());
        continue;
      }
      return std::unexpected(acquired.error());
    }
    ActiveConnection conn = std::move(*acquired);
    const bool reused = conn.reused;
    const NativeHandle fd = conn.cnx.fd();
    // The fd is registered with the event loop lazily, on the first would-block wait (armLoop). A reused
    // keep-alive connection is already the loop's registered fd, so it costs no syscall; a request whose
    // I/O never blocks touches the loop not at all.

    const auto now = SteadyClock::now();
    const auto connectDeadline = now + _config.connectTimeout;
    const auto ioDeadline = now + _config.requestTimeout;
    bool requestSent = false;

    // Drive the connection to a full exchange: finish the (TLS) connect, ensure a protocol engine, then hand
    // it the connection. `requestSent` flips to true as soon as any request byte is written, even when the
    // exchange later returns an error. Any step short-circuits to its HttpClientErrc.
    HttpClientResult result =
        finishConnect(conn, req.isTlsRequest(), connectDeadline)
            .and_then([&] { return ensureProtocolHandler(conn); })
            .and_then([&] { return conn.proto.exchange(*this, *conn.transport, fd, req, ioDeadline, requestSent); });

    if (result) {
      // A retryable status (e.g. 429 / 503) is a *successful* exchange we choose to retry: back off (honoring
      // a delta-seconds Retry-After when present) and try again, discarding this response.
      if (canBackoff() && std::ranges::find(retry.retryStatuses, result->status()) != retry.retryStatuses.end()) {
        RetryConfig::Duration delay = computedBackoff();
        if (retry.honorRetryAfter) {
          const RetryConfig::Duration ra =
              ParseRetryAfter(result->headerValueOrEmpty(http::RetryAfter), retry.maxDelay);
          if (ra >= RetryConfig::Duration::zero()) {
            delay = ra;
          }
        }
        // The connection may still be reusable (keep-alive); hand it back so the retry can reuse it.
        if (_config.keepAlive && conn.proto.keepAlive()) {
          releaseConnection(req, std::move(conn));
        } else {
          dropConnection(conn);
        }
        sleepBackoff(delay);
        continue;
      }
      if (_config.keepAlive && conn.proto.keepAlive()) {
        releaseConnection(req, std::move(conn));
      } else {
        dropConnection(conn);  // not reusable: unregister its fd before the socket closes
      }
      return result;
    }

    dropConnection(conn);  // close the socket / transport (and unregister its fd from the loop)
    if (reused && !requestSent) {
      continue;  // stale keep-alive race, nothing sent yet: free retry on a fresh connection (no backoff)
    }
    // Genuine failure on a connection we cannot reuse. A pre-send failure is always safe to retry; a
    // post-send failure only for an idempotent method when the caller explicitly opted in (a re-submission).
    const bool safeToRetry = !requestSent || (retry.retryIdempotentAfterSend && http::IsIdempotent(req.method()));
    if (canBackoff() && safeToRetry) {
      sleepBackoff(computedBackoff());
      continue;
    }
    return result;  // terminal failure
  }
}

bool HttpClient::cacheEligible(const HttpRequest& req) const noexcept {
  // TODO: can we activate cache for file payloads?
  return _config.cache.enabled() && http::IsMethodSet(_config.cache.methods, req.method()) && !req.hasBodyFile();
}

std::string_view HttpClient::buildCacheKey(const HttpRequest& req) {
  if (req.hasNoExternalPayload()) {
    // Optim - all data of the request is in the first buffer, so we can use it directly as the cache key without
    // copying.
    return req._data;
  }

  assert(!req.hasBodyFile());

  // Layout: [1 method-idx byte][url]['\n'][headers]['\n'][body]. The two '\n' separators keep the key
  // unambiguous (a URL never contains a raw newline, and the flat header block ends each line with CRLF), so
  // distinct (url, headers, body) triples can never collide by concatenation.
  _cacheKeyScratch.clear();
  _cacheKeyScratch.reserve(req._data.size() + req.bodySize());
  _cacheKeyScratch.unchecked_append(req._data);
  _cacheKeyScratch.unchecked_append(req.bodyInMemory());

  return _cacheKeyScratch;
}

void HttpClient::pruneExpiredCache(SteadyClock::time_point now) {
  const auto refresh = _config.cache.refreshPeriod;
  for (auto it = _cache.begin(); it != _cache.end();) {
    if (std::chrono::duration_cast<HttpClientConfig::Duration>(now - it->second.lastUpdated) >= refresh) {
      it = _cache.erase(it);
    } else {
      ++it;
    }
  }
}

HttpResponse* HttpClient::cacheLookupFresh(std::string_view key) {
  // Amortized housekeeping: every so often sweep expired entries so a cache of one-shot URLs does not keep
  // dead entries around until it hits maxEntries. Cheap relative to a network round trip.
  static constexpr uint32_t kCachePruneInterval = 256;
  if (++_cachePruneCounter >= kCachePruneInterval) {
    _cachePruneCounter = 0;
    pruneExpiredCache(SteadyClock::now());
  }
  auto it = _cache.find(key);
  if (it == _cache.end()) {
    return nullptr;  // miss
  }
  if (std::chrono::duration_cast<HttpClientConfig::Duration>(SteadyClock::now() - it->second.lastUpdated) >=
      _config.cache.refreshPeriod) {
    return nullptr;  // stale: caller refetches and overwrites this entry
  }
  return &it->second.response;
}

void HttpClient::cacheStore(std::string_view key, const HttpResponse& resp) {
  const auto now = SteadyClock::now();

  auto cloned = resp.cloneFinalized();
  auto [it, inserted] = _cache.try_emplace(key, std::move(cloned), now);

  if (!inserted) {
    // The above move actually did not happen, so it's safe to move again here.
    it->second.response = std::move(cloned);
    it->second.lastUpdated = now;
    return;
  }

  if (_cache.size() > _config.cache.maxEntries) {
    pruneExpiredCache(now);
    if (_cache.size() > _config.cache.maxEntries) {
      _cache.erase(std::ranges::min_element(_cache, {}, [](const auto& kv) { return kv.second.lastUpdated; }));
    }
  }
}

HttpClientResult HttpClient::requestUncached(HttpRequest&& req) {
  uint32_t redirectsLeft = _config.maxRedirects;

  while (true) {
    // performExchange owns the (idempotent-safe) transparent retry of a stale pooled connection; any
    // failure it surfaces here is terminal.
    HttpClientResult result = performExchange(req);
    if (!result) {
      return result;
    }
    const HttpResponse& resp = *result;

    if (!_config.followRedirects || !IsRedirect(resp.status()) || redirectsLeft == 0) {
      return result;
    }
    const std::string_view location = resp.headerValueOrEmpty(http::Location);
    if (location.empty()) {
      return result;  // 3xx without a Location: hand the response back rather than following.
    }

    // Method/body rewriting per RFC 7231.
    const http::StatusCode code = resp.status();
    const http::Method method = req.method();

    if (code == http::StatusCodeSeeOther ||
        ((code == http::StatusCodeMovedPermanently || code == http::StatusCodeFound) && method != http::Method::GET &&
         method != http::Method::HEAD)) {
      req.body(std::string_view{});
      req.method(http::Method::GET);
    }

    if (!req.resolveRedirect(location)) {
      return std::unexpected(HttpClientErrc::invalidUrl);
    }

    --redirectsLeft;
    _telemetry.counterAdd("aeronet.http_requests.redirects", 1);
  }
}

void HttpClient::maybeCompressRequestBody(HttpRequest& req) {
  const HttpClientConfig::RequestCompression& rc = _config.requestCompression;
  if (!rc.enabled()) {
    return;
  }
  if (IsEncodingEnabled(rc.encoding)) {
    const internal::CompressResponseResult result =
        internal::HttpCodec::TryCompressBody(_codec.compressionState, rc.encoding, req);

    switch (result) {
      case internal::CompressResponseResult::Uncompressed:
        break;
      case internal::CompressResponseResult::Compressed:
        _telemetry.counterAdd("aeronet.http_requests.compression.total", 1);
        break;
      case internal::CompressResponseResult::ExceedsMaxRatio:
        _telemetry.counterAdd("aeronet.http_requests.compression.exceeds_max_ratio_total", 1);
        break;
      default:
        assert(result == internal::CompressResponseResult::Error);
        _telemetry.counterAdd("aeronet.http_requests.compression.errors_total", 1);
        break;
    }
  }
}

}  // namespace aeronet
