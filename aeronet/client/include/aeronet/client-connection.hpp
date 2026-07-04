#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

#ifdef AERONET_ENABLE_HTTP2
#include <memory>
#endif

#include "aeronet/http-client-error.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

class HttpClient;
class ClientRequest;
class ITransport;
class Url;

#ifdef AERONET_ENABLE_HTTP2
struct Http2Config;
#endif

// Result of an HttpClient request: the HttpResponse on success, or an HttpClientErrc on failure. The
// success state carries a normal HttpResponse even for non-2xx statuses; only transport/protocol failures
// land in the error state.
using HttpClientResult = std::expected<HttpResponse, HttpClientErrc>;

namespace internal {

#ifdef AERONET_ENABLE_HTTP2
class Http2ClientEngine;  // defined in http2-connection.cpp (client module): Http2Connection + exchange state
#endif

// Per-connection protocol engine: the seam between HttpClient's (protocol-agnostic) connection
// management -- DNS/connect, pooling, redirects, retries -- and the wire protocol. One instance lives
// alongside each pooled connection.
//
// A handler borrows the resources it needs straight from the owning HttpClient passed to exchange(): the
// event loop (HttpClient::waitIo), the lazily-created (de)compression codec and the two reusable scratch
// buffers. The HttpClient owns all of these once and reuses them across every connection/request; the
// client is single-threaded and runs one exchange at a time, so sharing the scratch buffers is safe. The
// HttpClient is passed in rather than stored, so a handler holds no back-reference and the HttpClient stays
// movable.
//
// Two engines exist: HTTP/1.1 (stateless between exchanges, no allocation) and -- with
// AERONET_ENABLE_HTTP2 -- HTTP/2, whose per-connection state (HPACK tables, flow-control windows, stream
// ids) lives in a single heap-allocated Http2ClientEngine that travels with the pooled connection.
// Dispatch is a switch on `Type` (no virtual dispatch).
class ClientConnection {
 public:
  enum class Type : uint8_t { Empty, Http11, Http2 };

#ifdef AERONET_ENABLE_HTTP2
  // All special members are defined out-of-line (in http2-connection.cpp) because the header only
  // forward-declares the engine type behind the unique_ptr.
  explicit ClientConnection(Type type = Type::Empty) noexcept;

  // Construct the HTTP/2 engine (Type::Http2) with the client's HTTP/2 SETTINGS. The engine allocation is
  // the only per-connection setup cost.
  explicit ClientConnection(const Http2Config& http2Config);

  ClientConnection(const ClientConnection&) = delete;
  ClientConnection& operator=(const ClientConnection&) = delete;
  ClientConnection(ClientConnection&&) noexcept;
  ClientConnection& operator=(ClientConnection&&) noexcept;
  ~ClientConnection();
#else
  explicit ClientConnection(Type type = Type::Empty) noexcept : _type(type) {}
#endif

  [[nodiscard]] bool empty() const noexcept { return _type == Type::Empty; }

  // Perform a single request/response exchange over `transport`, returning the HttpResponse or an
  // HttpClientErrc on transport failure (never throws). `method` / `dropBody` carry redirect rewriting
  // without copying the (move-only) request. The owning `client` is borrowed for its event loop / scratch
  // buffers / codec, not stored. `requestSent` is set to true (even when the call later returns an error) as
  // soon as any request byte reaches the transport, so the caller can tell a pre-send failure (safe to
  // retry) from a post-send one (never retried, to avoid re-submitting a non-idempotent request).
  [[nodiscard]] HttpClientResult exchange(HttpClient& client, ITransport& transport, NativeHandle fd, const Url& url,
                                          const ClientRequest& req, http::Method method, bool dropBody,
                                          SteadyClock::time_point ioDeadline, bool& requestSent) {
#ifdef AERONET_ENABLE_HTTP2
    if (_type == Type::Http2) {
      return exchangeForHttp2(client, transport, fd, url, req, method, dropBody, ioDeadline, requestSent);
    }
#endif
    return exchangeForHttp11(client, transport, fd, url, req, method, dropBody, ioDeadline, requestSent);
  }

  // Whether the connection may be returned to the idle pool for a later exchange (verdict of the most
  // recent exchange: keep-alive plus bounded framing for HTTP/1.1; connection still open, no GOAWAY, for
  // HTTP/2).
  [[nodiscard]] bool keepAlive() const noexcept { return _keepAlive; }

#ifdef AERONET_ENABLE_HTTP2
  // Whether a pooled connection can host one more exchange. Always true for HTTP/1.1 (an idle pooled
  // connection is by definition free); for HTTP/2 the engine is consulted (no GOAWAY seen, stream ids not
  // exhausted, stream budget left). The pool checks this before reusing a connection so the 1:1
  // connection<->request assumption stays out of the pool itself.
  [[nodiscard]] bool canTakeAnotherStream() const noexcept;

  void reset() noexcept;
#else
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static) -- non-static to match the HTTP/2 build
  [[nodiscard]] bool canTakeAnotherStream() const noexcept { return true; }

  void reset() noexcept { _type = Type::Empty; }
#endif

 private:
  [[nodiscard]] HttpClientResult exchangeForHttp11(HttpClient& client, ITransport& transport, NativeHandle fd,
                                                   const Url& url, const ClientRequest& req, http::Method method,
                                                   bool dropBody, SteadyClock::time_point ioDeadline,
                                                   bool& requestSent);

  // Build the request head into client.requestBuffer() and return the body to send separately (a view into
  // `req` or the codec's compression buffer, never copied into the head buffer) -- unless the body is small
  // enough to be folded into the head buffer for a single write, in which case an empty view is returned.
  [[nodiscard]] static std::string_view buildRequestBytesForHttp11(HttpClient& client, const Url& url,
                                                                   const ClientRequest& req, http::Method method,
                                                                   bool dropBody);

  // Apply the shared opt-in outbound request-body compression policy (identical for HTTP/1.1 and HTTP/2).
  // Returns the body to send: on success the codec's reusable compression buffer, with `contentEncoding` set
  // to the applied coding token (the builder then emits Content-Encoding and rewrites Content-Length to the
  // compressed size); otherwise `body` is returned unchanged and `contentEncoding` is left empty. `body` must
  // already be empty when the request body is dropped. The codec is materialized lazily (only when a body is
  // actually compressed), so codec-free requests still pay nothing.
  [[nodiscard]] static std::string_view maybeCompressRequestBody(HttpClient& client, std::string_view body,
                                                                 bool hasContentEncoding, bool hasTransferEncoding,
                                                                 std::string_view& contentEncoding);

  // Write the request head followed by the body, pumping the event loop on would-block. While both buffers
  // are still pending they are sent with a single scatter write (writev / ordered TLS write) so the body is
  // never copied into the head buffer. Resumes correctly across partial writes spanning the two. Sets
  // `requestSent` to true as soon as any byte reaches the transport (the request can no longer be retried).
  // Returns an empty result on success or an HttpClientErrc on write failure / timeout.
  [[nodiscard]] static std::expected<void, HttpClientErrc> writeAllForHttp11(HttpClient& client, ITransport& transport,
                                                                             NativeHandle fd, std::string_view head,
                                                                             std::string_view body,
                                                                             SteadyClock::time_point deadline,
                                                                             bool& requestSent);

#ifdef AERONET_ENABLE_HTTP2
  [[nodiscard]] HttpClientResult exchangeForHttp2(HttpClient& client, ITransport& transport, NativeHandle fd,
                                                  const Url& url, const ClientRequest& req, http::Method method,
                                                  bool dropBody, SteadyClock::time_point ioDeadline, bool& requestSent);

  // Build the request header block (pseudo-headers first, then regular headers, all names lowercased --
  // RFC 9113 §8.2/§8.3) as flat "name: value\r\n" lines into client.requestBuffer(), ready for
  // Http2Connection::sendHeaders. Connection-specific headers are dropped and Host becomes :authority.
  // Returns the body to send in DATA frames (a view into `req` or the codec's compression buffer).
  [[nodiscard]] static std::string_view buildHeaderBlockForHttp2(HttpClient& client, const Url& url,
                                                                 const ClientRequest& req, http::Method method,
                                                                 bool dropBody);

  std::unique_ptr<Http2ClientEngine> _h2;  // engaged iff _type == Type::Http2
#endif
  Type _type{Type::Empty};
  bool _keepAlive{false};
};

}  // namespace internal
}  // namespace aeronet
