#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/encoding.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

class HttpRequest;

namespace internal {

/// Abstract transport backend for HttpResponseWriter.
///
/// The writer delegates all protocol-specific operations (header serialization,
/// body framing, stream end) to an IWriterTransport implementation:
///  - Http1WriterTransport: HTTP/1.1 chunked / fixed-length framing over a TCP socket.
///  - Http2WriterTransport: HTTP/2 HEADERS + DATA frames over an h2 stream.
///
/// Implementations must be stack-allocated alongside the writer; lifetimes are tied
/// to the streaming handler invocation scope.
class IWriterTransport {
 public:
  virtual ~IWriterTransport() = default;

  /// Emit the response headers to the peer.
  /// Called lazily on the first body write or on end().
  /// @param response  The HttpResponse containing accumulated headers/status.
  ///                  The transport may mutate it (e.g. add protocol-specific headers).
  /// @param request   The originating request (needed for middleware / CORS).
  /// @param compressionActivated  Whether streaming compression was activated.
  /// @param compressionFormat     The negotiated compression encoding.
  /// @param declaredLength  The declared Content-Length (0 = chunked/unknown).
  /// @param isHead    Whether the request is a HEAD request.
  /// @return true on success, false if the connection/stream is dead.
  virtual bool emitHeaders(HttpResponse& response, const HttpRequest& request, bool compressionActivated,
                           Encoding compressionFormat, std::size_t declaredLength, bool isHead) = 0;

  /// Emit a body data chunk.
  /// The transport applies the appropriate framing (HTTP/1.1 chunked encoding, HTTP/2 DATA frames).
  /// @param data  Raw body bytes (already compressed if compression is active).
  /// @return true on success (data queued), false on backpressure / fatal error.
  virtual bool emitData(std::string_view data) = 0;

  /// Emit the end of the response stream.
  /// For HTTP/1.1: emits the final zero-length chunk and optional trailers.
  /// For HTTP/2: emits trailers as a HEADERS frame with END_STREAM, or a DATA frame with END_STREAM.
  /// @param trailers  Pre-formatted trailer data (may be empty).
  /// @return true on success, false on error.
  virtual bool emitEnd(RawChars trailers) = 0;

  /// Check whether the underlying connection / stream is still alive.
  [[nodiscard]] virtual bool isAlive() const = 0;

  /// Return the identifier integer for log messages.
  [[nodiscard]] virtual uint32_t logId() const = 0;

 protected:
  IWriterTransport() = default;
};

}  // namespace internal
}  // namespace aeronet
