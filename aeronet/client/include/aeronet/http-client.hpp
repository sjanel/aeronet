#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

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
}  // namespace internal

// Synchronous HTTP/1.1 client built on aeronet's non-blocking transport + event-loop bricks.
//
// Highlights:
//   * Plain HTTP and (when AERONET_ENABLE_OPENSSL is set) HTTPS via the shared TlsTransport.
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
// NOTE: This is the first, deliberately simple iteration. Coroutine integration with a running
// server event loop and a native HTTP/2 client are tracked in docs/ROADMAP.md. The wire protocol lives
// behind the internal::ClientConnection seam (see acquireConnection / performExchange), so an HTTP/2
// engine slots in beside the HTTP/1.1 one without touching the connection-pool / redirect / retry code.
//
// HttpClient implements internal::ClientHost (privately): protocol handlers borrow its event loop,
// reusable scratch buffers and codec through that thin interface instead of the whole HttpClient.
class HttpClient : private internal::ClientHost {
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

  [[nodiscard]] const HttpClientConfig& config() const noexcept override { return _config; }

  // Drop all idle pooled connections (e.g. after a server restart). In-flight requests unaffected.
  void clearIdleConnections() { _idle.clear(); }

 private:
  // A live transport (plain or TLS) plus the socket it owns.
  struct ActiveConnection {
    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(cnx); }

    void reset() noexcept {
      transport.reset();
      idleSince = {};
      cnx = {};
      proto.reset();
      protocol = ClientProtocol::Http1_1;
      reused = false;
    }

    std::unique_ptr<ITransport> transport;
    SteadyClock::time_point idleSince;  // when this connection was returned to the idle pool
    // Wire-protocol engine for this connection (HTTP/1.1 today). Created lazily once the protocol is known
    // (after ALPN), then pooled with the connection so a reused connection keeps its handler/state.
    Connection cnx;
    internal::ClientConnection proto;
    ClientProtocol protocol{ClientProtocol::Http1_1};  // negotiated via ALPN (https) or assumed (http)
    bool reused{false};                                // taken from the idle pool
  };

  // Perform a single (non-redirect) exchange against an absolute URL, returning the HttpResponse or an
  // HttpClientErrc on transport failure. `method` / `dropBody` carry redirect rewriting without copying the
  // (move-only) request.
  HttpClientResult performExchange(const Url& url, const ClientRequest& req, http::Method method, bool dropBody);

  // Acquire a connection for the origin: reuse an idle pooled one or establish a fresh one.
  std::expected<ActiveConnection, HttpClientErrc> acquireConnection(const Url& url);

  std::expected<ActiveConnection, HttpClientErrc> connectNew(const Url& url);

  void releaseConnection(const Url& url, ActiveConnection&& conn);

  // Drive the TLS handshake to completion (the TCP connect is already established by connectNew), then
  // resolve the negotiated application protocol (ALPN for https; HTTP/1.1 otherwise) into conn.protocol.
  std::expected<void, HttpClientErrc> finishConnect(ActiveConnection& conn, const Url& url,
                                                    SteadyClock::time_point deadline);

  // Create conn.proto for conn.protocol if it is not already present (fresh connection). Returns
  // HttpClientErrc::protocolUnsupported when the negotiated protocol has no engine yet (e.g. ALPN "h2").
  static std::expected<void, HttpClientErrc> EnsureProtocolHandler(ActiveConnection& conn);

  // --- internal::ClientHost: resources protocol engines borrow (see client-connection.hpp) ---

  // Block (up to the deadline) until fd signals one of the interest events. Returns true if ready.
  bool waitIo(NativeHandle fd, EventBmp interest, SteadyClock::time_point deadline) override;

  // Lazily-created compression/decompression state (decoders, encoders, scratch buffers). Only built on
  // the first request that actually needs a codec, so codec-free usage pays nothing.
  internal::HttpClientCodec& codec() override;

  [[nodiscard]] RawChars& requestBuffer() noexcept override { return _requestBuffer; }
  [[nodiscard]] RawChars& responseBuffer() noexcept override { return _responseBuffer; }

#ifdef AERONET_ENABLE_OPENSSL
  internal::HttpClientTlsContext& tlsContext();
#endif

  // Draw a uniform value in [0, 1) from the backoff jitter PRNG (a tiny xorshift; quality is irrelevant
  // here, only spread). Only consulted when RetryConfig::jitter > 0, so a jitter-free client is fully
  // deterministic. Defined in http-client.cpp.
  double nextJitterUnit() noexcept;

  HttpClientConfig _config;
  EventLoop _loop;
  uint64_t _jitterState{0x9E3779B97F4A7C15ULL};  // backoff jitter PRNG state (non-zero seed)
  // Idle keep-alive connections keyed by origin ("scheme://host:port"); transparent string_view lookup.
  flat_hash_map<RawChars32, vector<ActiveConnection>, CityHash, std::equal_to<>> _idle;
  RawChars _requestBuffer;   // reused across requests to avoid reallocations
  RawChars _responseBuffer;  // reused across requests: raw bytes are read straight into its tail
  std::unique_ptr<internal::HttpClientCodec> _codec;  // compression/decompression state (lazily created)
#ifdef AERONET_ENABLE_OPENSSL
  std::unique_ptr<internal::HttpClientTlsContext> _tls;
#endif
};

}  // namespace aeronet
