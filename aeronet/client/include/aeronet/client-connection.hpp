#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

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

// Result of an HttpClient request: the HttpResponse on success, or an HttpClientErrc on failure. The
// success state carries a normal HttpResponse even for non-2xx statuses; only transport/protocol failures
// land in the error state.
using HttpClientResult = std::expected<HttpResponse, HttpClientErrc>;

namespace internal {

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
// HTTP/1.1 is implemented today; an HTTP/2 engine will dispatch off the same `Type` enum (no virtual
// dispatch) and slot into the pool without touching the surrounding connection-management machinery.
class ClientConnection {
 public:
  enum class Type : uint8_t { Empty, Http11 };

  explicit ClientConnection(Type type = Type::Empty) noexcept : _type(type) {}

  [[nodiscard]] bool empty() const noexcept { return _type == Type::Empty; }

  void reset() noexcept { _type = Type::Empty; }

  // Perform a single request/response exchange over `transport`, returning the HttpResponse or an
  // HttpClientErrc on transport failure (never throws). `method` / `dropBody` carry redirect rewriting
  // without copying the (move-only) request. The owning `client` is borrowed for its event loop / scratch
  // buffers / codec, not stored. `requestSent` is set to true (even when the call later returns an error) as
  // soon as any request byte reaches the transport, so the caller can tell a pre-send failure (safe to
  // retry) from a post-send one (never retried, to avoid re-submitting a non-idempotent request).
  [[nodiscard]] HttpClientResult exchange(HttpClient& client, ITransport& transport, NativeHandle fd, const Url& url,
                                          const ClientRequest& req, http::Method method, bool dropBody,
                                          SteadyClock::time_point ioDeadline, bool& requestSent) {
    // hardcoded for http1/1 for now, but in the future we can switch on _type here and implement in dedicated
    // translation units for each protocol.
    return exchangeForHttp11(client, transport, fd, url, req, method, dropBody, ioDeadline, requestSent);
  }

  // Whether the connection may be returned to the idle pool for a later exchange (verdict of the most
  // recent exchange: keep-alive plus bounded framing).
  [[nodiscard]] bool keepAlive() const noexcept { return _keepAlive; }

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

  Type _type{Type::Empty};
  bool _keepAlive{false};
};

}  // namespace internal
}  // namespace aeronet
