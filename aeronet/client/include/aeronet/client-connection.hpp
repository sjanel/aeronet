#pragma once

#include <expected>

#include "aeronet/event.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

class HttpClientConfig;
class ClientRequest;
class ITransport;
class Url;

// Result of an HttpClient request: the HttpResponse on success, or an HttpClientErrc on failure. The
// success state carries a normal HttpResponse even for non-2xx statuses; only transport/protocol failures
// land in the error state.
using HttpClientResult = std::expected<HttpResponse, HttpClientErrc>;

namespace internal {

struct HttpClientCodec;

// Services a per-connection protocol handler borrows from the owning HttpClient: the event loop (driven
// through waitIo), the lazily-created (de)compression codec and the two reusable scratch buffers. The
// HttpClient owns all of these once and reuses them across every connection/request; the client is
// single-threaded and runs one exchange at a time, so sharing the scratch buffers is safe. Passing this
// thin interface (instead of HttpClient itself) keeps protocol handlers -- including a future HTTP/2 one
// living in its own translation unit -- decoupled from HttpClient internals.
class ClientHost {
 public:
  virtual ~ClientHost() = default;

  // Block (up to `deadline`) until `fd` signals one of `interest` (pumping the event loop). false on timeout.
  [[nodiscard]] virtual bool waitIo(NativeHandle fd, EventBmp interest, SteadyClock::time_point deadline) = 0;

  [[nodiscard]] virtual HttpClientCodec& codec() = 0;
  [[nodiscard]] virtual const HttpClientConfig& config() const noexcept = 0;
  [[nodiscard]] virtual RawChars& requestBuffer() noexcept = 0;   // reused scratch for the request head
  [[nodiscard]] virtual RawChars& responseBuffer() noexcept = 0;  // reused scratch for received bytes
};

// Per-connection protocol engine: the seam between HttpClient's (protocol-agnostic) connection
// management -- DNS/connect, pooling, redirects, retries -- and the wire protocol. One instance lives
// alongside each pooled connection. HTTP/1.1 (Http11Connection) is implemented today; an HTTP/2 engine
// will derive from this same interface and slot into the pool without touching the surrounding machinery.
class ClientConnection {
 public:
  virtual ~ClientConnection() = default;

  // Perform a single request/response exchange over `transport`, returning the HttpResponse or an
  // HttpClientErrc on transport failure (never throws). `method` / `dropBody` carry redirect rewriting
  // without copying the (move-only) request. The owning HttpClient is passed as `host` rather than stored,
  // so handlers hold no back-reference and the HttpClient stays movable. `requestSent` is set to true (even
  // when the call later returns an error) as soon as any request byte reaches the transport, so the caller
  // can tell a pre-send failure (safe to retry) from a post-send one (never retried, to avoid re-submitting
  // a non-idempotent request).
  [[nodiscard]] virtual HttpClientResult exchange(ClientHost& host, ITransport& transport, NativeHandle fd,
                                                  const Url& url, const ClientRequest& req, http::Method method,
                                                  bool dropBody, SteadyClock::time_point ioDeadline,
                                                  bool& requestSent) = 0;

  // Whether the connection may be returned to the idle pool for a later exchange (verdict of the most
  // recent exchange: keep-alive plus bounded framing).
  [[nodiscard]] virtual bool keepAlive() const noexcept = 0;

  // Whether another concurrent stream could be multiplexed over this connection right now. Always false for
  // HTTP/1.1 (one exchange at a time); an HTTP/2 engine may return true. Lets the pool avoid assuming a 1:1
  // connection<->request model.
  [[nodiscard]] virtual bool canTakeAnotherStream() const noexcept = 0;
};

}  // namespace internal
}  // namespace aeronet
