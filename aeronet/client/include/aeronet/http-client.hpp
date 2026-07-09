#pragma once

#include <amc/type_traits.hpp>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string_view>
#include <type_traits>

#include "aeronet/city-hash.hpp"
#include "aeronet/client-connection.hpp"
#include "aeronet/client-protocol.hpp"
#include "aeronet/client-request.hpp"
#include "aeronet/connection.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/url.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

namespace internal {
struct HttpClientTlsContext;  // defined in http-client.cpp (OpenSSL-backed, optional)
struct HttpClientCodec;       // defined in http-client.cpp (compression/decompression state + scratch buffers)
#ifdef AERONET_ENABLE_HTTP2
class Http2ClientEngine;  // defined in http2-connection.cpp (native HTTP/2 client engine)
#endif
}  // namespace internal

// Synchronous HTTP/1.1 + HTTP/2 client built on aeronet's non-blocking transport + event-loop bricks.
//
// Highlights:
//   * Plain HTTP and (when AERONET_ENABLE_OPENSSL is set) HTTPS via the shared TlsTransport.
//   * Native HTTP/2 (when AERONET_ENABLE_HTTP2 is set), reusing the server's HPACK + frame codecs:
//     negotiated via ALPN over https, or spoken directly over plain http with prior knowledge --
//     see HttpClientConfig::httpVersion (Auto / Http1_1 / Http2).
//   * Per-origin keep-alive connection pooling with bounded idle reuse.
//   * Automatic redirect following and connection-level retry.
//   * Reuses HttpResponse as the response/request field container (no bespoke header/body types).
//
// The returned response is an HttpResponse. Received headers are preserved losslessly via rawHeader(),
// except Content-Type, Content-Length and Transfer-Encoding, which are normalized: Content-Type and the
// decoded Content-Length are reconstructed via body(), and chunked framing is de-framed away. Every
// other header (Connection, Date, Trailer, Upgrade, Location, ETag, Set-Cookie, custom ...) is
// available as usual.
//
// Thread-safety: a single HttpClient instance is NOT thread-safe (it owns one event loop and a
// connection pool). Use one instance per thread, or guard externally.
//
// Error model: every request returns an HttpClientResult (std::expected<HttpResponse, HttpClientErrc>).
// A per-request runtime failure (invalid URL, DNS/connect failure, timeout, TLS error, malformed/oversized
// response, ...) lands in the error state and is never thrown. A non-2xx HTTP status is NOT an error: it is
// a normal HttpResponse in the success state. Exceptions are reserved for hard setup errors detected when
// building the client / TLS context (misconfiguration, certificate failures): those still throw
// HttpClientException (or std::logic_error when https is requested in a build without OpenSSL).
//
// NOTE: This is a deliberately simple synchronous iteration: one exchange runs at a time, so an HTTP/2
// connection carries one stream at a time (the pool is multiplexing-aware, nothing more). Coroutine
// integration with a running server event loop is tracked in docs/ROADMAP.md. The wire protocol lives
// behind the internal::ClientConnection seam (see acquireConnection / performExchange), where the HTTP/2
// engine sits beside the HTTP/1.1 one without touching the connection-pool / redirect / retry code.
//
// Protocol handlers borrow HttpClient's event loop, reusable scratch buffers and codec directly from the
// HttpClient handed to internal::ClientConnection::exchange (see performExchange). ClientConnection is a
// friend so those resources stay private to HttpClient's public API.
class HttpClient {
 public:
  explicit HttpClient(HttpClientConfig config = {});

  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  HttpClient(HttpClient&&) noexcept = default;
  HttpClient& operator=(HttpClient&&) noexcept = default;

  ~HttpClient();

  // Execute an arbitrary request (method + url + headers + body), following redirects per config.
  HttpClientResult request(const ClientRequest& req);

  // Convenience verbs.
  HttpClientResult get(std::string_view url);
  HttpClientResult head(std::string_view url);
  HttpClientResult post(std::string_view url, std::string_view body,
                        std::string_view contentType = "application/octet-stream");
  HttpClientResult put(std::string_view url, std::string_view body,
                       std::string_view contentType = "application/octet-stream");
  HttpClientResult del(std::string_view url);

  [[nodiscard]] const HttpClientConfig& config() const noexcept { return _config; }

  // Drop all idle pooled connections (e.g. after a server restart). In-flight requests unaffected.
  void clearIdleConnections();

  // Evict every entry from the built-in response cache, forcing the next eligible request to refetch. No-op
  // when the cache is disabled. See HttpClientConfig::cache.
  void clearResponseCache() noexcept { _cache.clear(); }

 private:
  // A live transport (plain or TLS) plus the socket it owns.
  struct ActiveConnection {
    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(cnx); }

    void reset() noexcept {
      transport.reset();
      idleSince = {};
      proto.reset();
      cnx = {};
      protocol = ClientProtocol::Http1_1;
      reused = false;
    }

    using trivially_relocatable = std::bool_constant<amc::is_trivially_relocatable_v<internal::ClientConnection> &&
                                                     amc::is_trivially_relocatable_v<Connection>>::type;

    std::unique_ptr<ITransport> transport;
    SteadyClock::time_point idleSince;  // when this connection was returned to the idle pool
    internal::ClientConnection proto;
    // Wire-protocol engine for this connection (HTTP/1.1 or HTTP/2). Created lazily once the protocol is
    // known (after ALPN), then pooled with the connection so a reused connection keeps its handler/state
    // (for HTTP/2: negotiated settings, HPACK dynamic tables, stream ids).
    Connection cnx;
    ClientProtocol protocol{ClientProtocol::Http1_1};  // negotiated via ALPN (https) or assumed (http)
    bool reused{false};                                // taken from the idle pool
  };

  // Run a request end to end (URL parse + redirect following), bypassing the response cache. request() wraps
  // this with the cache lookup/store when caching is enabled and the request is eligible.
  HttpClientResult requestUncached(const ClientRequest& req);

  // Perform a single (non-redirect) exchange against an absolute URL, returning the HttpResponse or an
  // HttpClientErrc on transport failure. `method` / `dropBody` carry redirect rewriting without copying the
  // (move-only) request.
  HttpClientResult performExchange(const Url& url, const ClientRequest& req, http::Method method, bool dropBody);

  // --- Built-in response cache (see HttpClientConfig::cache) ---
  // Whether `req` is eligible for caching (cache enabled and its method is in cache.methods).
  [[nodiscard]] bool cacheEligible(const ClientRequest& req) const noexcept;
  // Build the cache key (method + url + headers + body) into the reusable _cacheKeyScratch buffer and return
  // a view of it. The view stays valid until the next buildCacheKey call (requestUncached never touches it).
  std::string_view buildCacheKey(const ClientRequest& req);
  // Return the cached response for `key` if present and still fresh, else nullptr (a miss or a stale entry).
  // Amortized periodic pruning of expired entries happens here.
  HttpResponse* cacheLookupFresh(std::string_view key);
  // Store (a deep copy of) `resp` under `key`, refreshing an existing entry or inserting a new one; enforces
  // the cache.maxEntries bound (prune expired, then evict the least-recently-refreshed entry).
  void cacheStore(std::string_view key, const HttpResponse& resp);
  // Erase every cache entry at least refreshPeriod old.
  void pruneExpiredCache(SteadyClock::time_point now);

  // Acquire a connection for the origin: reuse an idle pooled one or establish a fresh one.
  std::expected<ActiveConnection, HttpClientErrc> acquireConnection(const Url& url);

  std::expected<ActiveConnection, HttpClientErrc> connectNew(const Url& url);

  // Whether a forward proxy is configured (see HttpClientConfig::withProxy). When true, connectNew connects
  // to the proxy and, for an https origin, opens a CONNECT tunnel before the TLS handshake.
  [[nodiscard]] bool usesProxy() const noexcept { return !_proxyHost.empty(); }

  // Open an HTTP CONNECT tunnel to `url`'s origin through the configured proxy, on the already-connected raw
  // socket `fd` (wrapped by the throwaway plain `transport`). Drives the request/response on the event loop
  // up to `deadline`; returns an empty result once the proxy answers 2xx, or an HttpClientErrc otherwise.
  std::expected<void, HttpClientErrc> establishProxyTunnel(ITransport& transport, NativeHandle fd, const Url& url,
                                                           SteadyClock::time_point deadline);

  void releaseConnection(const Url& url, ActiveConnection&& conn);

  // Drive the TLS handshake to completion (the TCP connect is already established by connectNew), then
  // resolve the negotiated application protocol (ALPN for https; HTTP/1.1 otherwise) into conn.protocol.
  std::expected<void, HttpClientErrc> finishConnect(ActiveConnection& conn, const Url& url,
                                                    SteadyClock::time_point deadline);

  // Create conn.proto for conn.protocol if it is not already present (fresh connection). Returns
  // HttpClientErrc::protocolUnsupported when the negotiated protocol has no engine in this build
  // (ClientProtocol::Http2 without AERONET_ENABLE_HTTP2).
  std::expected<void, HttpClientErrc> ensureProtocolHandler(ActiveConnection& conn) const;

  // --- Resources protocol engines borrow (see client-connection.hpp) ---
  // internal::ClientConnection (and the HTTP/2 engine it owns) read these (private) accessors during an
  // exchange, so they are friends.
  friend class internal::ClientConnection;
#ifdef AERONET_ENABLE_HTTP2
  friend class internal::Http2ClientEngine;
#endif

  // Block (up to the deadline) until fd signals one of the interest events. Returns true if ready.
  bool waitIo(NativeHandle fd, EventBmp interest, SteadyClock::time_point deadline);

  // Ensure the event loop is watching `fd` for exactly `interest`, reusing the existing registration when
  // possible. The client keeps a single fd registered at a time (the one it is currently driving): a
  // request that keeps hitting the same keep-alive connection reuses the registration untouched (no
  // epoll_ctl on the hot path), and switching to a different connection swaps it (del old + add new).
  // Returns false if the loop rejects the registration.
  bool armLoop(NativeHandle fd, EventBmp interest);

  // Drop `fd` from the event loop if it is the currently-registered one. Called right before a socket is
  // closed so the registration never outlives the fd (and the cache never matches a recycled fd number).
  void unregisterIfCurrent(NativeHandle fd) noexcept;

  // Close a connection we are done with: unregister its fd from the loop (if current), then reset it.
  void dropConnection(ActiveConnection& conn) noexcept;

  // Unregister every still-registered fd in `bucket` from the loop, then clear the bucket.
  void dropIdleBucket(vector<ActiveConnection>& bucket) noexcept;

  // Lazily-created compression/decompression state (decoders, encoders, scratch buffers). Only built on
  // the first request that actually needs a codec, so codec-free usage pays nothing.
  internal::HttpClientCodec& codec();

  // requestBuffer() and bodyBuffer() intentionally return the same buffer (_reqBodyScratch): a request is
  // always fully written before its response body is de-framed, so the two roles never overlap in time. The
  // distinct names keep call sites self-documenting; see the _reqBodyScratch declaration.
  [[nodiscard]] RawChars& requestBuffer() noexcept { return _reqBodyScratch; }
  [[nodiscard]] RawChars& responseBuffer() noexcept { return _responseBuffer; }
  [[nodiscard]] RawChars& bodyBuffer() noexcept { return _reqBodyScratch; }

#ifdef AERONET_ENABLE_OPENSSL
  internal::HttpClientTlsContext& tlsContext();
#endif

  // Draw a uniform value in [0, 1) from the backoff jitter PRNG (a tiny xorshift; quality is irrelevant
  // here, only spread). Only consulted when RetryConfig::jitter > 0, so a jitter-free client is fully
  // deterministic. Defined in http-client.cpp.
  double nextJitterUnit() noexcept;

  HttpClientConfig _config;
  EventLoop _loop;
  // Single-fd registration cache for _loop: the fd currently watched (kInvalidHandle if none) and the
  // interest it is watched for. Keeping only the actively-driven fd registered lets a keep-alive
  // connection be reused across requests without re-adding / re-arming the loop on every request, while
  // never leaving idle pooled fds registered (which, level-triggered, could spin the poll loop).
  NativeHandle _loopFd{kInvalidHandle};
  uint32_t _cachePruneCounter{0};  // amortizes the periodic sweep of expired cache entries
  EventBmp _loopInterest{0};
  uint16_t _proxyPort{0};                        // forward-proxy port (see _proxyHost)
  uint64_t _jitterState{0x9E3779B97F4A7C15ULL};  // backoff jitter PRNG state (non-zero seed)

  // A cached response plus the timestamp it was last refreshed (for TTL expiry).
  struct CacheEntry {
    HttpResponse response;
    SteadyClock::time_point lastUpdated;
  };

  // Idle keep-alive connections keyed by origin ("scheme://host:port"); transparent string_view lookup.
  flat_hash_map<RawChars32, vector<ActiveConnection>, CityHash, std::equal_to<>> _idle;
  // Built-in response cache keyed by request identity (method + url + headers + body); transparent
  // string_view lookup. Empty / unused unless HttpClientConfig::cache is enabled.
  flat_hash_map<RawChars32, CacheEntry, CityHash, std::equal_to<>> _cache;
  RawChars _cacheKeyScratch;  // reused buffer to build lookup keys without per-request allocation
  // Dual-role request / response-body scratch: one allocation shared between two exchange phases that are
  // never live at once. Phase 1 holds the outgoing request (HTTP/1.1 head, HTTP/2 header block, or proxy
  // CONNECT line); once it is fully written, phase 2 reuses it as the HTTP/1.1 chunked de-framing target
  // (borrowed by ResponseParser, which clears it in reset()). Kept distinct from _responseBuffer because
  // chunked de-framing reads from that receive buffer while writing into this one. HTTP/2 only uses the
  // phase-1 role. Reused across requests so a keep-alive connection never re-grows the allocation. Exposed
  // through the requestBuffer() / bodyBuffer() accessors, whose names document the two roles.
  RawChars _reqBodyScratch;
  RawChars _responseBuffer;  // reused across requests: raw bytes are read straight into its tail
  // Forward-proxy host, empty when no proxy is configured (parsed once from HttpClientConfig::proxyUrl at
  // construction). Kept with one spare trailing byte so it can be handed to ConnectTCP (which transiently
  // null-terminates its host span) exactly like a Url host.
  RawChars _proxyHost;
  std::unique_ptr<internal::HttpClientCodec> _codec;  // compression/decompression state (lazily created)
#ifdef AERONET_ENABLE_OPENSSL
  std::unique_ptr<internal::HttpClientTlsContext> _tls;
#endif
};

}  // namespace aeronet
