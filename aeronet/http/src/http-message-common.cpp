#include "aeronet/http-message-common.hpp"

#include <cassert>
#include <cstddef>
#include <stdexcept>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/log.hpp"

namespace aeronet {

void ThrowInvalidContentTypeHeaderValue(std::string_view contentType) {
  if (http::IsValidHeaderValue(contentType)) {
    // Printable and still invalid: too short, so tiny and safe to log.
    log::critical("Invalid Content-Type header value '{}': shorter than the minimum of {} characters", contentType,
                  http::ContentTypeMinLen);
  } else {
    // The value may come from untrusted data and hold CRLF (forging log lines): log only the first forbidden byte.
    std::size_t pos = 0;
    while (http::IsValidHeaderValue(contentType.substr(pos, 1))) {
      ++pos;
      assert(pos < contentType.size());
    }
    log::critical("Invalid Content-Type header value of size {}: forbidden byte 0x{:02x} at offset {}",
                  contentType.size(), static_cast<unsigned>(static_cast<unsigned char>(contentType[pos])), pos);
  }
  throw std::invalid_argument("HTTP content-type header value is invalid");
}

void ThrowSetBodyAfterTrailers() {
  log::critical("Cannot set the body of an HTTP message after trailers have been added");
  throw std::logic_error("Cannot set body after trailers have been added");
}

}  // namespace aeronet
