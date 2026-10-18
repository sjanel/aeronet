#pragma once

#include <algorithm>

#include "aeronet/http-constants.hpp"
#include "aeronet/lower-ascii-key.hpp"

namespace aeronet::http {

// Centralized rule for HTTP response headers the user may not set directly (normal or streaming path).
// These are managed by aeronet itself: automatically emitted (Date, Content-Type, Content-Length, Connection,
// Transfer-Encoding, Trailer when the response has trailers, Upgrade when a protocol switch such as WebSocket is
// accepted) or meaningless in a response (TE), so a user-supplied value would be ambiguous or contradict what is sent.
// Keeping this here allows future optimization of storage layout without scattering the logic.
// You can use 'static_assert' to make sure at compilation time that the header you are about to insert is not
// reserved. The list of reserved headers is unlikely to change in the future, but they are mostly technical /
// framework headers that aeronet manages internally and probably not very interesting for the client.
// Example:
//     static_assert(!aeronet::http::IsReservedResponseHeader("x-my-header")); // OK
//     static_assert(!aeronet::http::IsReservedResponseHeader("content-length")); // Not OK
constexpr bool IsReservedResponseHeader(LowerAsciiKey name) noexcept {
  static constexpr LowerAsciiKey kHeaders[]{
      http::Connection, http::ContentLength, http::ContentType,      http::Date,
      http::TE,         http::Trailer,       http::TransferEncoding, http::Upgrade,
  };
  return std::ranges::any_of(kHeaders, [name](LowerAsciiKey candidate) { return name == candidate; });
}

// Same as IsReservedResponseHeader but for request headers.
constexpr bool IsReservedOrForbiddenRequestHeader(LowerAsciiKey name) noexcept {
  static constexpr LowerAsciiKey kHeaders[]{
      http::ContentLength, http::ContentType,      http::Expect,  http::Host, http::TE,
      http::Trailer,       http::TransferEncoding, http::Upgrade,
  };
  return std::ranges::any_of(kHeaders, [name](LowerAsciiKey candidate) { return name == candidate; });
}

// RFC 7230 §4.1.2: Certain headers MUST NOT appear in trailers (chunked transfer encoding).
// This function checks if a header name is forbidden in trailer context.
// Forbidden trailer headers include:
//   - Transfer-Encoding, Content-Length, Host (message framing and routing)
//   - Trailer itself (no recursion)
//   - Cache-Control, Expires, Pragma, Vary (caching directives need to be known early)
//   - Authorization, Set-Cookie, Cookie (security/authentication must be in head)
//   - Content-Encoding, Content-Type, Content-Range (content metadata)
//   - Expect, Range, If-* conditionals, TE (request control headers)
// This is a conservative list for safety and correctness.
constexpr bool IsForbiddenTrailerHeader(LowerAsciiKey trailerName) noexcept {
  static constexpr LowerAsciiKey kForbiddenTrailers[]{
      "authorization",
      "cache-control",
      "content-encoding",
      http::ContentLength,
      "content-range",
      http::ContentType,
      "cookie",
      "expect",
      "expires",
      "host",
      "if-match",
      "if-modified-since",
      "if-none-match",
      "if-unmodified-since",
      "pragma",
      "range",
      "set-cookie",
      "te",
      "trailer",
      "transfer-encoding",
      "vary",
  };

  return std::ranges::any_of(kForbiddenTrailers,
                             [trailerName](LowerAsciiKey candidate) { return trailerName == candidate; });
}

}  // namespace aeronet::http