#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "aeronet/client-protocol.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/concatenated-headers.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/retry-config.hpp"
#include "aeronet/static-concatenated-strings.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"
#include "aeronet/telemetry-config.hpp"
#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-config.hpp"  // reuse the server's TLSConfig::Version type for API symmetry
#endif

namespace aeronet {

namespace internal {
// First compiled-in content coding (in aeronet's preference order) used by default when the client
// is asked to auto-compress request bodies. Resolves to Encoding::none when no codec is compiled in,
// in which case request auto-compression stays a no-op even if enabled.
constexpr Encoding DefaultRequestEncoding() {
#ifdef AERONET_ENABLE_ZSTD
  return Encoding::zstd;
#elifdef AERONET_ENABLE_BROTLI
  return Encoding::br;
#elifdef AERONET_ENABLE_ZLIB
  return Encoding::gzip;
#else
  return Encoding::none;
#endif
}
}  // namespace internal

// Configuration for HttpClient. Scalar knobs are public fields (ordered by descending size to
// minimise padding); the variable-length strings are packed into a single contiguous buffer
// (one allocation, trivially relocatable) and exposed through a small fluent method API, mirroring
// how aeronet's other config objects (e.g. TLSConfig) store their strings.
class HttpClientConfig {
 public:
  using Duration = std::chrono::milliseconds;

  // Maximum time to establish a TCP connection (and, for https, complete the TLS handshake).
  Duration connectTimeout{std::chrono::seconds{10}};
  // Maximum time for a single request/response exchange once connected (excludes connect time).
  Duration requestTimeout{std::chrono::seconds{30}};

  // Max idle age of a pooled keep-alive connection: when older, it is dropped instead of reused (the
  // origin server may have closed it after its own keep-alive timeout, saving a doomed reuse attempt).
  // 0 disables expiry. Mirrors HttpServerConfig::keepAliveTimeout. Default: 30s.
  Duration keepAliveTimeout{std::chrono::seconds{30}};

  // Hard cap on the total response size (headers + decoded body) accepted before aborting.
  std::size_t maxResponseBytes{64UL * 1024UL * 1024UL};

  uint32_t maxRedirects{5};

  // Maximum idle connections retained per origin in the pool.
  uint32_t maxIdleConnectionsPerHost{8};

  // Will add all the headers defined here in all client requests, if not explicitly set by the user for a given
  // response. Defaults to a list of one entry "user-agent: aeronet".
  // Each added header MUST have their key and value separated by (exactly one) aeronet::http::HeaderSep.
  // The maximum number of global headers is kMaxGlobalHeaders.
  ConcatenatedHeaders globalHeaders{{"user-agent: aeronet"}};

  // Transparent retry + exponential-backoff policy (subsumes the previous `maxRetries` knob). The default
  // (`retry.maxAttempts == 1`) keeps the historical behaviour: the always-safe pre-send stale-pool retry
  // stays on, while no extra backoff retries happen. See RetryConfig for the full semantics.
  RetryConfig retry;

  // HTTP/2 SETTINGS / limits used by the native HTTP/2 engine, mirroring HttpServerConfig::http2 (the
  // server-only knobs -- enableH2c, enableH2cUpgrade, enablePush, enablePriority -- are ignored here).
  // Only consulted when a connection actually speaks HTTP/2 (see `httpVersion`) and requires a build with
  // AERONET_ENABLE_HTTP2.
  Http2Config http2;

  // --- Built-in response cache for safe / idempotent requests ---
  //
  // A simple in-process TTL cache: while a cached response is
  // younger than `refreshPeriod`, a repeated eligible request is served straight from memory with no network
  // round trip; once older, the next request refetches and refreshes the entry. It is a pure time-based
  // cache and deliberately does NOT interpret HTTP cache-control semantics (`Cache-Control`, `ETag`,
  // `Vary`, `Age`, ...): the refresh period is authoritative. Entries are keyed by request method + URL +
  // request headers + body, so requests that differ in any of those (e.g. a different `Authorization`) never
  // share an entry. File bodies are identified without reading their contents, using the byte range, and
  // current descriptor metadata. Only successful (2xx) responses are stored, and only for methods in `methods` (default
  // GET + HEAD). Disabled by default (`refreshPeriod == 0`); enable it by setting a positive refresh period
  // (`Duration::max()` caches for the client's whole lifetime). The cache lives in the HttpClient and shares
  // its single-threaded assumption.
  struct RequestCache {
    // Whether the cache is active. A zero refresh period keeps it off (opt-in).
    [[nodiscard]] bool enabled() const noexcept { return refreshPeriod > Duration::zero(); }

    // Time-to-live of a cached response: an entry at least this old is refetched on the next request. 0
    // disables the cache; Duration::max() never expires an entry.
    Duration refreshPeriod{Duration::zero()};
    // Hard cap on the number of cached entries (memory guard). When inserting into a full cache, expired
    // entries are pruned first, then -- if still full -- the least-recently-refreshed entry is evicted.
    std::uint32_t maxEntries{1024};
    // Request methods eligible for caching. Must be a subset of the safe methods (GET / HEAD / OPTIONS):
    // caching an unsafe or non-idempotent method's response is nonsensical and is rejected by validate().
    http::MethodBmp methods{http::Method::GET | http::Method::HEAD};
  };
  RequestCache cache;

  bool followRedirects{true};
  bool keepAlive{true};

  // Whether to add a `Trailer` header to every request containing trailers.
  bool addTrailerHeader{true};

  // Which HTTP version(s) the client may speak (see HttpVersionMode). Auto (the default) negotiates
  // HTTP/2 via ALPN over https when the build has HTTP/2 support and falls back to HTTP/1.1 everywhere
  // else; Http2 requires HTTP/2 (prior-knowledge h2c over plain http); Http1_1 disables HTTP/2 entirely.
  HttpVersionMode httpVersion{HttpVersionMode::Auto};

  // TCP_NODELAY (disable Nagle). A request/response client almost always wants the request flushed
  // immediately rather than batched, so Auto enables it. Mirrors HttpServerConfig::tcpNoDelay.
  // Default: Auto.
  TcpNoDelayMode tcpNoDelay{TcpNoDelayMode::Auto};
#ifdef AERONET_ENABLE_OPENSSL
  // Verify the server certificate chain and hostname for https requests.
  bool tlsVerifyPeer{true};

  // Negotiated TLS protocol bounds. minVersion defaults to TLS 1.2 (secure default, preserving the
  // client's previous hard-coded floor); an unset maxVersion lets the library pick the highest it
  // supports. Mirrors the min/max version knobs of HttpServerConfig::tls.
  TLSConfig::Version tlsMinVersion{TLSConfig::TLS_1_2};
  TLSConfig::Version tlsMaxVersion;  // unset => library default (highest supported)
#endif

  // --- Automatic response decompression (gzip / deflate / br / zstd) ---
  //
  // Reuses the very same DecompressionConfig knobs as the server's inbound request decompression
  // (size caps, ratio guards, streaming threshold, ...). When enabled (the default whenever at least
  // one codec is compiled in) the client transparently decodes a Content-Encoding'd response body,
  // replaces it with the decoded bytes and drops the Content-Encoding header. In addition, unless the
  // user provides an explicit defaultAcceptEncoding, the client advertises the codecs it can decode in
  // the Accept-Encoding request header so origins know they may compress.
  DecompressionConfig decompression;

  // --- Automatic request body compression (large outbound payloads, e.g. big POST/PUT) ---
  //
  // Opt-in (disabled by default): not every origin understands a Content-Encoding'd request body, so
  // enabling this is the caller's explicit choice. When enabled, an outbound body whose size is at
  // least `codec.minBytes` is compressed with `encoding` (adding a Content-Encoding header and an
  // updated Content-Length) provided the result satisfies `codec.maxCompressRatio`; otherwise the body
  // is sent verbatim. The body is never compressed when the request already carries a Content-Encoding
  // or Transfer-Encoding header.
  struct RequestCompression {
    // Request body compression is active iff a (non-identity) codec is selected.
    [[nodiscard]] bool enabled() const noexcept { return encoding != Encoding::none; }

    // Codec used for outbound bodies. Encoding::none (the default) disables request body compression;
    // any other value selects that codec and must be compiled in (otherwise HttpClient construction
    // throws). There is no separate on/off flag: the encoding is the single source of truth.
    Encoding encoding{Encoding::none};
    // Per-algorithm levels and the `minBytes` / `maxCompressRatio` thresholds, mirroring the server's
    // CompressionConfig so the two ends share one mental model (and one set of bricks).
    CompressionConfig codec;
  };
  RequestCompression requestCompression;

  // For requests with captured bodies smaller than this threshold, HttpRequest will concatenate the captured body
  // contents with the head in the same buffer. This can be efficient for small
  // bodies because it improves cache locality. Only used in HTTP/1.1.
  // Larger bodies will be kept separate. Default: 1 KiB.
  std::size_t minCapturedBodySize{1024UL};

  // Telemetry configuration (OpenTelemetry tracing + DogStatsD metrics)
  TelemetryConfig telemetry;

  void validate() const;

  HttpClientConfig& addGlobalHeader(const http::Header& header) {
    globalHeaders.appendAsHttp1Header(header.name(), header.value());
    return *this;
  }

  HttpClientConfig& withGlobalHeaders(std::span<const http::Header> headers);

  // Default Accept-Encoding advertised when the request does not set one. When left empty and response
  // decompression is enabled, the client auto-advertises the codecs it can actually decode; set this to
  // override that (e.g. "identity" to opt out, or a curated list). Responses are still auto-decoded when
  // `decompression.enable` is set, regardless of what was advertised.
  [[nodiscard]] std::string_view defaultAcceptEncoding() const { return _strings[kAcceptEncoding]; }
  HttpClientConfig& withDefaultAcceptEncoding(std::string_view acceptEncoding) {
    _strings.set(kAcceptEncoding, acceptEncoding);
    return *this;
  }

  // Enable/disable automatic response decompression (see `decompression`).
  HttpClientConfig& withDecompression(bool enable = true) {
    decompression.enable = enable;
    return *this;
  }

  // Enable automatic request body compression with the default compiled-in codec (or disable it).
  HttpClientConfig& withRequestCompression(bool enable = true) {
    requestCompression.encoding = enable ? internal::DefaultRequestEncoding() : Encoding::none;
    return *this;
  }
  // Select the codec for automatic request body compression (Encoding::none disables it).
  HttpClientConfig& withRequestCompression(Encoding encoding) {
    requestCompression.encoding = encoding;
    return *this;
  }

  // Threshold below which an outbound body is folded into the head buffer for a single contiguous write.
  HttpClientConfig& withMinCapturedBodySize(std::size_t minBytes) {
    minCapturedBodySize = minBytes;
    return *this;
  }

  // Set TCP_NODELAY mode for new connections. Default: Auto (enabled for this request/response client).
  HttpClientConfig& withTcpNoDelayMode(TcpNoDelayMode mode) {
    tcpNoDelay = mode;
    return *this;
  }

  // Convenience: enable/disable TCP_NODELAY explicitly.
  HttpClientConfig& withTcpNoDelay(bool on = true) {
    tcpNoDelay = on ? TcpNoDelayMode::Enabled : TcpNoDelayMode::Disabled;
    return *this;
  }

  // Set the max idle age of pooled keep-alive connections (0 disables expiry).
  HttpClientConfig& withKeepAliveTimeout(Duration timeout) {
    keepAliveTimeout = timeout;
    return *this;
  }

  // Set the transparent retry + backoff policy.
  HttpClientConfig& withRetry(RetryConfig retryConfig) {
    retry = std::move(retryConfig);
    return *this;
  }

  // Enable (or, with a zero period, disable) the built-in response cache and set its TTL (see `cache`).
  HttpClientConfig& withCache(Duration refreshPeriod) {
    cache.refreshPeriod = refreshPeriod;
    return *this;
  }

  // Set the maximum number of cached responses retained before eviction (see `cache.maxEntries`).
  HttpClientConfig& withCacheMaxEntries(std::uint32_t maxEntries) {
    cache.maxEntries = maxEntries;
    return *this;
  }

  // Restrict which request methods are eligible for caching (subset of GET / HEAD / OPTIONS). Pass a single
  // method or an OR-combination (e.g. http::Method::GET | http::Method::HEAD).
  HttpClientConfig& withCacheMethods(http::MethodBmp methods) {
    cache.methods = methods;
    return *this;
  }
  HttpClientConfig& withCacheMethods(http::Method method) {
    cache.methods = static_cast<http::MethodBmp>(method);
    return *this;
  }

  // Select which HTTP version(s) the client may speak (Auto / Http1_1 / Http2).
  HttpClientConfig& withHttpVersion(HttpVersionMode mode) {
    httpVersion = mode;
    return *this;
  }

  // Set the HTTP/2 SETTINGS / limits used by the native HTTP/2 engine.
  HttpClientConfig& withHttp2Config(Http2Config http2Config) {
    http2 = http2Config;
    return *this;
  }

  // --- Forward proxy (HTTP CONNECT tunnelling) ---
  //
  // Route every request through an HTTP forward proxy given as an absolute URL ("http://host:port"; a bare
  // "host[:port]" is also accepted and assumed to be http, defaulting to port 80). For an https origin the
  // client opens a tunnel to the origin with an HTTP CONNECT request to the proxy, then performs the origin
  // TLS handshake through it; for a plain http origin the request is sent to the proxy in absolute-form
  // ("GET http://origin/path HTTP/1.1"). Only cleartext (http) proxies are supported -- HttpClient
  // construction throws HttpClientException for an https proxy URL or a malformed one. Empty disables it.
  [[nodiscard]] bool hasProxy() const { return !_strings[kProxyUrl].empty(); }
  [[nodiscard]] std::string_view proxyUrl() const { return _strings[kProxyUrl]; }
  HttpClientConfig& withProxy(std::string_view proxyUrl) {
    _strings.set(kProxyUrl, proxyUrl);
    return *this;
  }

  // Optional CA bundle used to verify the origin TLS handshake when the proxy intercepts (re-signs) it --
  // e.g. a debugging proxy such as mitmproxy that presents certificates signed by its own CA. When set it
  // overrides the default trust store / tlsCaFile for proxied https requests. Empty keeps the normal TLS
  // trust settings. Only meaningful for https origins in an OpenSSL build (ignored otherwise).
  [[nodiscard]] std::string_view proxyCaFile() const { return _strings[kProxyCaFile]; }
  [[nodiscard]] const char* proxyCaFileCStr() const { return _strings.c_str(kProxyCaFile); }
  HttpClientConfig& withProxy(std::string_view proxyUrl, std::string_view caFile) {
    _strings.set(kProxyUrl, proxyUrl);
    _strings.set(kProxyCaFile, caFile);
    return *this;
  }

#ifdef AERONET_ENABLE_OPENSSL
  // Optional CA bundle file / directory. When both empty, the system default trust store is used.
  [[nodiscard]] std::string_view tlsCaFile() const { return _strings[kCaFile]; }
  [[nodiscard]] const char* tlsCaFileCStr() const { return _strings.c_str(kCaFile); }
  HttpClientConfig& withTlsCaFile(std::string_view caFile) {
    _strings.set(kCaFile, caFile);
    return *this;
  }

  [[nodiscard]] std::string_view tlsCaPath() const { return _strings[kCaPath]; }
  [[nodiscard]] const char* tlsCaPathCStr() const { return _strings.c_str(kCaPath); }
  HttpClientConfig& withTlsCaPath(std::string_view caPath) {
    _strings.set(kCaPath, caPath);
    return *this;
  }

  // Set minimum / maximum negotiated TLS version (e.g. TLSConfig::TLS_1_3).
  HttpClientConfig& withTlsMinVersion(TLSConfig::Version version) {
    tlsMinVersion = version;
    return *this;
  }
  HttpClientConfig& withTlsMaxVersion(TLSConfig::Version version) {
    tlsMaxVersion = version;
    return *this;
  }

  // Optional OpenSSL cipher list (TLS 1.2 and below). Empty leaves the library default.
  [[nodiscard]] std::string_view tlsCipherList() const { return _strings[kCipherList]; }
  [[nodiscard]] const char* tlsCipherListCStr() const { return _strings.c_str(kCipherList); }
  HttpClientConfig& withTlsCipherList(std::string_view cipherList) {
    _strings.set(kCipherList, cipherList);
    return *this;
  }

  // Client certificate + key for mutual TLS (mTLS), provided as file paths. Both empty disables mTLS.
  [[nodiscard]] std::string_view tlsClientCertFile() const { return _strings[kClientCertFile]; }
  [[nodiscard]] const char* tlsClientCertFileCStr() const { return _strings.c_str(kClientCertFile); }
  [[nodiscard]] std::string_view tlsClientKeyFile() const { return _strings[kClientKeyFile]; }
  [[nodiscard]] const char* tlsClientKeyFileCStr() const { return _strings.c_str(kClientKeyFile); }
  HttpClientConfig& withTlsClientCertKeyFile(std::string_view certFile, std::string_view keyFile) {
    _strings.set(kClientCertFile, certFile);
    _strings.set(kClientKeyFile, keyFile);
    return *this;
  }

  // Client certificate + key for mutual TLS, provided as in-memory PEM. Both empty disables mTLS.
  [[nodiscard]] std::string_view tlsClientCertPem() const { return _strings[kClientCertPem]; }
  [[nodiscard]] std::string_view tlsClientKeyPem() const { return _strings[kClientKeyPem]; }
  HttpClientConfig& withTlsClientCertKeyMemory(std::string_view certPem, std::string_view keyPem) {
    _strings.set(kClientCertPem, certPem);
    _strings.set(kClientKeyPem, keyPem);
    return *this;
  }
#endif

 private:
  enum : uint8_t {
    kAcceptEncoding,
    kCaFile,
    kCaPath,
    kCipherList,
    kClientCertFile,
    kClientKeyFile,
    kClientCertPem,
    kClientKeyPem,
    kProxyUrl,
    kProxyCaFile,
    kNbStrings,
  };

  // [acceptEncoding, caFile, caPath, cipherList, clientCertFile, clientKeyFile, clientCertPem,
  //  clientKeyPem, proxyUrl, proxyCaFile]
  StaticConcatenatedStrings<kNbStrings, uint32_t> _strings;
};

}  // namespace aeronet
