#include "aeronet/http-client.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
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
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/retry-config.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/url.hpp"
#include "aeronet/zerocopy-mode.hpp"
#include "http-client-codec.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/prov_ssl.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>
#include <openssl/types.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "aeronet/tls-config.hpp"
#include "aeronet/tls-raii.hpp"
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

}  // namespace

#ifdef AERONET_ENABLE_OPENSSL
namespace internal {

namespace {

// Map an aeronet TLSConfig::Version onto the OpenSSL protocol constant (0 == unset / unsupported).
int ToOpenSslTlsVersion(TLSConfig::Version ver) {
  if (ver == TLSConfig::TLS_1_2) {
    return TLS1_2_VERSION;
  }
#ifdef TLS1_3_VERSION
  if (ver == TLSConfig::TLS_1_3) {
    return TLS1_3_VERSION;
  }
#endif
  return 0;
}

// Load a client certificate + private key (mutual TLS) into the context, from in-memory PEM if
// provided, otherwise from file paths. No-op when neither is configured.
void LoadClientCertificate(SSL_CTX* ctx, const HttpClientConfig& cfg) {
  const std::string_view certPem = cfg.tlsClientCertPem();
  const std::string_view keyPem = cfg.tlsClientKeyPem();
  if (!certPem.empty() && !keyPem.empty()) {
    auto certBio = MakeMemBio(certPem.data(), static_cast<int>(certPem.size()));
    auto keyBio = MakeMemBio(keyPem.data(), static_cast<int>(keyPem.size()));
    // Wrap the parse results directly (rather than via MakeX509/MakePKey, which raise std::bad_alloc on
    // null) so a malformed PEM surfaces as the documented HttpClientException, not a misleading bad_alloc.
    X509Ptr certX509(::PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr), ::X509_free);
    PKeyPtr pkey(::PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr), ::EVP_PKEY_free);
    if (!certX509 || !pkey) {
      throw HttpClientException("Failed to parse in-memory client certificate or key");
    }
    if (::SSL_CTX_use_certificate(ctx, certX509.get()) != 1 || ::SSL_CTX_use_PrivateKey(ctx, pkey.get()) != 1) {
      throw HttpClientException("Failed to install in-memory client certificate");
    }
  } else if (!cfg.tlsClientCertFile().empty() && !cfg.tlsClientKeyFile().empty()) {
    if (::SSL_CTX_use_certificate_file(ctx, cfg.tlsClientCertFileCStr(), SSL_FILETYPE_PEM) != 1) {
      throw HttpClientException("Failed to load client certificate file");
    }
    if (::SSL_CTX_use_PrivateKey_file(ctx, cfg.tlsClientKeyFileCStr(), SSL_FILETYPE_PEM) != 1) {
      throw HttpClientException("Failed to load client private key file");
    }
  } else {
    return;  // no client certificate configured
  }
  if (::SSL_CTX_check_private_key(ctx) != 1) {
    throw HttpClientException("Client private key does not match the certificate");
  }
}

}  // namespace

// OpenSSL client context: one SSL_CTX shared by all connections of a single HttpClient.
struct HttpClientTlsContext {
  explicit HttpClientTlsContext(const HttpClientConfig& cfg)
      : ctx(::SSL_CTX_new(::TLS_client_method()), &::SSL_CTX_free) {
    cfg.validate();
    if (!ctx) {
      throw HttpClientException("SSL_CTX_new(TLS_client_method) failed");
    }
    const int minVersion = ToOpenSslTlsVersion(cfg.tlsMinVersion);
    if (minVersion != 0 && ::SSL_CTX_set_min_proto_version(ctx.get(), minVersion) != 1) {
      throw HttpClientException("Failed to set minimum TLS version");
    }
    if (cfg.tlsMaxVersion != TLSConfig::Version{}) {
      const int maxVersion = ToOpenSslTlsVersion(cfg.tlsMaxVersion);
      if (maxVersion == 0 || ::SSL_CTX_set_max_proto_version(ctx.get(), maxVersion) != 1) {
        throw HttpClientException("Failed to set maximum TLS version");
      }
    }
    if (!cfg.tlsCipherList().empty() && ::SSL_CTX_set_cipher_list(ctx.get(), cfg.tlsCipherListCStr()) != 1) {
      throw HttpClientException("Failed to set TLS cipher list");
    }
    LoadClientCertificate(ctx.get(), cfg);
    ::SSL_CTX_set_mode(ctx.get(), SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    // Advertise the application protocols we can actually speak via ALPN. Today only HTTP/1.1 ("\x08http/1.1"
    // in OpenSSL's length-prefixed wire format). When the HTTP/2 client engine lands, prepend "\x02h2" here
    // and add the ClientProtocol::Http2 branch in ensureProtocolHandler(); the negotiated protocol is read
    // back from SSL_get0_alpn_selected() in finishConnect(). SSL_CTX_set_alpn_protos returns 0 on success.
    static constexpr unsigned char kAlpnWire[]{8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    if (::SSL_CTX_set_alpn_protos(ctx.get(), kAlpnWire, sizeof(kAlpnWire)) != 0) {
      throw HttpClientException("Failed to set TLS ALPN protocols");
    }
    if (cfg.tlsVerifyPeer) {
      ::SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
      bool trustLoaded = false;
      if (!cfg.tlsCaFile().empty() || !cfg.tlsCaPath().empty()) {
        const char* caFile = cfg.tlsCaFile().empty() ? nullptr : cfg.tlsCaFileCStr();
        const char* caPath = cfg.tlsCaPath().empty() ? nullptr : cfg.tlsCaPathCStr();
        trustLoaded = ::SSL_CTX_load_verify_locations(ctx.get(), caFile, caPath) == 1;
        if (!trustLoaded) {
          throw HttpClientException("Failed to load TLS CA trust store");
        }
      } else {
        trustLoaded = ::SSL_CTX_set_default_verify_paths(ctx.get()) == 1;
        if (!trustLoaded) {
          throw HttpClientException("Failed to load default TLS trust store");
        }
      }
    } else {
      ::SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);
    }
  }

  // Build a TLS transport in client connect state, with SNI and (optionally) hostname verification.
  [[nodiscard]] std::unique_ptr<ITransport> makeTransport(NativeHandle fd, const char* pHost, bool verify) const {
    TlsTransport::SslPtr ssl(::SSL_new(ctx.get()), &::SSL_free);
    if (!ssl) {
      throw HttpClientException("SSL_new failed");
    }
    if (::SSL_set_fd(ssl.get(), static_cast<int>(fd)) != 1) {
      throw HttpClientException("SSL_set_fd failed");
    }
    // OpenSSL SNI / verification APIs need a null-terminated host string.
    // SNI (host must be a registered name, not an IP literal; OpenSSL ignores IPs here which is fine).
    ::SSL_set_tlsext_host_name(ssl.get(), pHost);
    if (verify) {
      ::SSL_set_hostflags(ssl.get(), X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
      if (::SSL_set1_host(ssl.get(), pHost) != 1) {
        throw HttpClientException("SSL_set1_host failed");
      }
    }
    ::SSL_set_connect_state(ssl.get());
    auto transport = std::make_unique<TlsTransport>(std::move(ssl), ~0U);
    transport->setUnderlyingFd(fd);
    return transport;
  }

  std::unique_ptr<SSL_CTX, void (*)(SSL_CTX*)> ctx;
};

}  // namespace internal
#endif

HttpClient::HttpClient(HttpClientConfig config) : _config(std::move(config)), _loop(CreateEventLoop()) {
  // Fail fast on inconsistent codec configuration (mirrors the server's validate-on-construct policy).
  _config.decompression.validate();  // no-op when disabled
  if (_config.requestCompression.enabled()) {
    _config.requestCompression.codec.validate();
#if !defined(AERONET_ENABLE_ZLIB) || !defined(AERONET_ENABLE_ZSTD) || !defined(AERONET_ENABLE_BROTLI)
    if (!IsEncodingEnabled(_config.requestCompression.encoding)) {
      throw HttpClientException("requestCompression.encoding is not a supported / compiled-in content coding");
    }
#endif
  }
}

HttpClient::~HttpClient() = default;

internal::HttpClientCodec& HttpClient::codec() {
  if (!_codec) {
    _codec = std::make_unique<internal::HttpClientCodec>(_config.requestCompression.codec);
  }
  return *_codec;
}

#ifdef AERONET_ENABLE_OPENSSL
internal::HttpClientTlsContext& HttpClient::tlsContext() {
  if (!_tls) {
    _tls = std::make_unique<internal::HttpClientTlsContext>(_config);
  }
  return *_tls;
}
#endif

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
  if (conn.valid()) {
    unregisterIfCurrent(conn.cnx.fd());
  }
  conn.reset();
}

void HttpClient::dropIdleBucket(vector<ActiveConnection>& bucket) noexcept {
  for (ActiveConnection& conn : bucket) {
    if (conn.valid()) {
      unregisterIfCurrent(conn.cnx.fd());
    }
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

std::expected<HttpClient::ActiveConnection, HttpClientErrc> HttpClient::connectNew(const Url& url) {
  // For https, build (and thereby validate) the shared TLS context up front -- before spending a connect on
  // it. A misconfiguration (bad cipher list, unusable cert/key, ...) is a hard setup error: it throws an
  // HttpClientException deterministically here, independently of whether the origin is even reachable.
  // The context is built once and reused; this is a no-op on every subsequent connect.
  if (url.tls()) {
#ifdef AERONET_ENABLE_OPENSSL
    tlsContext();
#else
    throw std::logic_error("https requested but aeronet was built without OpenSSL support");
#endif
  }

  // getaddrinfo (via ConnectTCP) needs writable host/port buffers with one spare byte at the end.
  // Build transient buffers here (only on a real connect, never on pool reuse).
  char portStr[std::numeric_limits<uint16_t>::digits10 + 2];
  [[maybe_unused]] const auto [portEnd, portEc] = std::to_chars(portStr, portStr + sizeof(portStr), url.port());
  assert(portEc == std::errc{});
  // Opt into ConnectTCP's blocking multi-address fallback (bounded by the connect timeout): a host that
  // resolves to several addresses (e.g. "localhost" -> ::1 then 127.0.0.1) must try them in turn instead
  // of committing to the first, whose non-blocking connect would otherwise hide a deferred ECONNREFUSED.
  const auto connectTimeoutMs = static_cast<int>(_config.connectTimeout.count());
  ConnectResult cr =
      ConnectTCP(std::span<char>(const_cast<char*>(url.host().data()), url.host().size()),
                 std::span<char>(portStr, static_cast<std::size_t>(portEnd - portStr)), 0, connectTimeoutMs);
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
#ifdef AERONET_ENABLE_OPENSSL
  if (url.tls()) {
    // The context is already built above; makeTransport only wraps the freshly connected fd.
    conn.transport = tlsContext().makeTransport(fd, url.hostCStr().c_str(), _config.tlsVerifyPeer);
  } else
#endif
  {
    conn.transport = std::make_unique<PlainTransport>(fd, ZerocopyMode::Disabled, ~0U);
  }
  return conn;
}

std::expected<HttpClient::ActiveConnection, HttpClientErrc> HttpClient::acquireConnection(const Url& url) {
  if (_config.keepAlive) {
    if (auto it = _idle.find(url.originKey()); it != _idle.end() && !it->second.empty()) {
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
        // we have just confirmed is still open.
        if (!IsConnectionStale(conn.cnx.fd())) {
          conn.reused = true;
          return conn;
        }
        dropConnection(conn);    // unregister + close the stale connection we popped
        dropIdleBucket(bucket);  // and its likely-stale siblings
      }
    }
  }
  return connectNew(url);
}

void HttpClient::releaseConnection(const Url& url, ActiveConnection&& conn) {
  assert(_config.keepAlive);
  if (!conn.valid()) {
    return;
  }
  // This is the only place the pool needs to own (and therefore allocate) an origin key: create the
  // bucket on first release for this origin so subsequent requests can actually reuse the connection.
  // The key is materialized once per distinct origin and never rebuilt; all other accesses are
  // transparent string_view lookups.
  auto& bucket = _idle.try_emplace(url.originKey()).first->second;
  if (bucket.size() >= _config.maxIdleConnectionsPerHost) {
    dropConnection(conn);  // pool full: unregister its fd from the loop before it closes
    return;
  }
  conn.idleSince = SteadyClock::now();  // stamp for idle-expiry on the next acquire
  bucket.emplace_back(std::move(conn));
}

std::expected<void, HttpClientErrc> HttpClient::finishConnect(ActiveConnection& conn, const Url& url,
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
#ifdef AERONET_ENABLE_OPENSSL
  // Resolve the negotiated application protocol from ALPN. Only https negotiates ALPN; plain HTTP (and an
  // empty / unknown selection) stays HTTP/1.1. This is the single point where conn.protocol is decided for
  // a fresh connection; a reused pooled connection keeps the protocol it negotiated originally.
  if (url.tls() && conn.transport) {
    const unsigned char* alpn = nullptr;
    unsigned int alpnLen = 0;
    ::SSL_get0_alpn_selected(static_cast<TlsTransport*>(conn.transport.get())->rawSsl(), &alpn, &alpnLen);
    if (alpn != nullptr && alpnLen != 0) {
      conn.protocol = ClientProtocolFromAlpnId(std::string_view(reinterpret_cast<const char*>(alpn), alpnLen));
    }
  }
#endif
  return {};
}

std::expected<void, HttpClientErrc> HttpClient::EnsureProtocolHandler(ActiveConnection& conn) {
  if (!conn.proto.empty()) {
    return {};  // reused from the pool: the protocol engine (and any per-connection state) travels with it
  }
  switch (conn.protocol) {
    case ClientProtocol::Http2:
      // We never advertise "h2" yet, so ALPN cannot select it; this is a defensive guard for the day the
      // advertised list grows before the engine is wired. Construct the HTTP/2 engine here at that point.
      return std::unexpected(HttpClientErrc::protocolUnsupported);
    case ClientProtocol::Http1_1:
      break;
  }
  conn.proto = internal::ClientConnection(internal::ClientConnection::Type::Http11);
  return {};
}

double HttpClient::nextJitterUnit() noexcept {
  // xorshift64* step; map the top 53 bits to a double in [0, 1). Quality is irrelevant here -- this only
  // spreads backoff delays so a fleet of clients does not retry in lockstep.
  _jitterState ^= _jitterState << 13;
  _jitterState ^= _jitterState >> 7;
  _jitterState ^= _jitterState << 17;
  return static_cast<double>(_jitterState >> 11) * (1.0 / 9007199254740992.0);  // 1 / 2^53
}

HttpClientResult HttpClient::performExchange(const Url& url, const ClientRequest& req, http::Method method,
                                             bool dropBody) {
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
    ++attempt;
  };
  // Computed exponential-backoff delay for the upcoming retry (jitter consulted only when enabled).
  const auto computedBackoff = [&] { return retry.delayFor(attempt, retry.jitter > 0.0F ? nextJitterUnit() : 0.0); };

  for (;;) {
    std::expected<ActiveConnection, HttpClientErrc> acquired = acquireConnection(url);
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
        finishConnect(conn, url, connectDeadline).and_then([&] { return EnsureProtocolHandler(conn); }).and_then([&] {
          return conn.proto.exchange(*this, *conn.transport, fd, url, req, method, dropBody, ioDeadline, requestSent);
        });

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
          releaseConnection(url, std::move(conn));
        } else {
          dropConnection(conn);
        }
        sleepBackoff(delay);
        continue;
      }
      if (_config.keepAlive && conn.proto.keepAlive()) {
        releaseConnection(url, std::move(conn));
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
    const bool safeToRetry = !requestSent || (retry.retryIdempotentAfterSend && http::IsIdempotent(method));
    if (canBackoff() && safeToRetry) {
      sleepBackoff(computedBackoff());
      continue;
    }
    return result;  // terminal failure
  }
}

HttpClientResult HttpClient::request(const ClientRequest& req) {
  std::expected<Url, HttpClientErrc> parsed = Url::Parse(req.url());
  if (!parsed) {
    return std::unexpected(parsed.error());  // malformed / unsupported URL
  }
  Url url = std::move(*parsed);

  // Redirect rewriting is tracked out-of-band so the (move-only) request never needs copying.
  http::Method method = req.method();
  bool dropBody = false;
  uint32_t redirectsLeft = _config.maxRedirects;

  for (;;) {
    // performExchange owns the (idempotent-safe) transparent retry of a stale pooled connection; any
    // failure it surfaces here is terminal.
    HttpClientResult result = performExchange(url, req, method, dropBody);
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
    // A malformed Location surfaces as HttpClientErrc::invalidUrl (propagated to the caller); it is not
    // expected on a nominal redirect.
    std::expected<Url, HttpClientErrc> next = url.resolveRedirect(location);
    if (!next) {
      return std::unexpected(next.error());
    }

    // Method/body rewriting per RFC 7231.
    const http::StatusCode code = resp.status();
    if (code == http::StatusCodeSeeOther ||
        ((code == http::StatusCodeMovedPermanently || code == http::StatusCodeFound) && method != http::Method::GET &&
         method != http::Method::HEAD)) {
      method = http::Method::GET;
      dropBody = true;
    }

    url = std::move(*next);
    --redirectsLeft;
  }
}

HttpClientResult HttpClient::get(std::string_view url) { return request(ClientRequest(http::Method::GET, url)); }

HttpClientResult HttpClient::head(std::string_view url) { return request(ClientRequest(http::Method::HEAD, url)); }

HttpClientResult HttpClient::post(std::string_view url, std::string_view body, std::string_view contentType) {
  ClientRequest req(http::Method::POST, url);
  req.body(body, contentType);
  return request(req);
}

HttpClientResult HttpClient::put(std::string_view url, std::string_view body, std::string_view contentType) {
  ClientRequest req(http::Method::PUT, url);
  req.body(body, contentType);
  return request(req);
}

HttpClientResult HttpClient::del(std::string_view url) { return request(ClientRequest(http::Method::DELETE, url)); }

}  // namespace aeronet
