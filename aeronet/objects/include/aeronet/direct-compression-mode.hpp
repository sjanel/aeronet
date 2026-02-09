#pragma once

#include <cstdint>

namespace aeronet {

// Defines the direct automatic compression mode for HttpResponse's inline bodies.
enum class DirectCompressionMode : std::uint8_t {

  // If the following conditions are true:
  //  - HttpResponse has been created with HttpRequest::makeResponse()
  //  - The HttpRequest has an Accept-Encoding header containing at least one supported encoding
  //  - A first call to body() or bodyAppend() (inline body sets / append) is made with a chunk of body data whose size
  //    is greater than or equal to CompressionConfig::minBytes and with a content type satisfying
  //    CompressionConfig::contentTypeAllowList (or empty)
  //
  // Then the HttpResponse will initiate a streaming compression of the body data using the best supported encoding from
  // the Accept-Encoding header, without waiting for the final body size to be known.
  // It is more efficient than the automatic compression happening at finalization step as it does not need an temporary
  // buffer to hold the uncompressed body. For captured bodies, as they are already in a separate buffer, this
  // optimization is not relevant and will not be applied (automatic compression at finalization step will still apply
  // if conditions are satisfied).
  Auto,

  // Never initiate a direct compression, even if above requirements are satisfied.
  Off,

  // Like Auto but not checking CompressionConfig requirements (accept-encoding header should still be present).
  // This is useful if you know in advance that the body will be compressible but that the first chunk will be smaller
  // than CompressionConfig::minBytes, or if you want to bypass content type checks.
  On
};

}  // namespace aeronet