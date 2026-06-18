#pragma once

#include <expected>
#include <string_view>

#include "aeronet/client-connection.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

class ClientRequest;
class ITransport;
class Url;

namespace internal {

// HTTP/1.1 protocol engine. Builds the request head (status line + headers) and body into the host's
// reusable scratch buffer, drives the (scatter) write, and parses the response with ResponseParser.
// Stateless apart from the keep-alive verdict of the most recent exchange.
class Http11Connection final : public ClientConnection {
 public:
  [[nodiscard]] HttpClientResult exchange(ClientHost& host, ITransport& transport, NativeHandle fd, const Url& url,
                                          const ClientRequest& req, http::Method method, bool dropBody,
                                          SteadyClock::time_point ioDeadline, bool& requestSent) override;

  [[nodiscard]] bool keepAlive() const noexcept override { return _keepAlive; }

  // HTTP/1.1 is strictly one exchange at a time.
  [[nodiscard]] bool canTakeAnotherStream() const noexcept override { return false; }

 private:
  // Build the request head into host.requestBuffer() and return the body to send separately (a view into
  // `req` or the codec's compression buffer, never copied into the head buffer) -- unless the body is small
  // enough to be folded into the head buffer for a single write, in which case an empty view is returned.
  [[nodiscard]] static std::string_view buildRequestBytes(ClientHost& host, const Url& url, const ClientRequest& req,
                                                          http::Method method, bool dropBody);

  // Write the request head followed by the body, pumping the event loop on would-block. While both buffers
  // are still pending they are sent with a single scatter write (writev / ordered TLS write) so the body is
  // never copied into the head buffer. Resumes correctly across partial writes spanning the two. Sets
  // `requestSent` to true as soon as any byte reaches the transport (the request can no longer be retried).
  // Returns an empty result on success or an HttpClientErrc on write failure / timeout.
  [[nodiscard]] static std::expected<void, HttpClientErrc> writeAll(ClientHost& host, ITransport& transport,
                                                                    NativeHandle fd, std::string_view head,
                                                                    std::string_view body,
                                                                    SteadyClock::time_point deadline,
                                                                    bool& requestSent);

  bool _keepAlive{false};
};

}  // namespace internal
}  // namespace aeronet
