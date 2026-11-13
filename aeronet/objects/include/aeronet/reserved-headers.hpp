#pragma once

#include <algorithm>
#include <string_view>

#include "aeronet/compiler-config.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/toupperlower.hpp"

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
// Checked on each header set by a handler: one comparison at most for most names, dispatched on the length. The names
// are compared as std::string_view of a length known in each case, which compiles to a few fixed-size loads.
AERONET_ALWAYS_INLINE constexpr bool IsReservedResponseHeader(LowerAsciiKey name) noexcept {
  static_assert(http::Trailer.size() == http::Upgrade.size());
  const std::string_view sv = name;
  switch (sv.size()) {
    case http::TE.size():
      return sv == http::TE.get();
    case http::Date.size():
      return sv == http::Date.get();
    case http::Trailer.size():
      return sv == http::Trailer.get() || sv == http::Upgrade.get();
    case http::Connection.size():
      return sv == http::Connection.get();
    case http::ContentType.size():
      return sv == http::ContentType.get();
    case http::ContentLength.size():
      return sv == http::ContentLength.get();
    case http::TransferEncoding.size():
      return sv == http::TransferEncoding.get();
    default:
      return false;
  }
}

// Same as IsReservedResponseHeader but for request headers.
constexpr bool IsReservedOrForbiddenRequestHeader(LowerAsciiKey name) noexcept {
  static constexpr LowerAsciiKey kHeaders[]{
      http::ContentLength, http::ContentType,      http::Host,    http::TE,
      http::Trailer,       http::TransferEncoding, http::Upgrade,
  };
  return std::ranges::any_of(kHeaders, [name](LowerAsciiKey candidate) { return name == candidate; });
}

// RFC 9110 §6.5.1: the fields needed before the content is received cannot be processed in trailers. This function
// checks if a header name is forbidden in trailer context:
//   - Transfer-Encoding, Content-Length, Host, Connection, Upgrade (message framing, routing and connection)
//   - Trailer itself (no recursion)
//   - Cache-Control, Expires, Pragma, Vary (caching directives need to be known early)
//   - Authorization, Proxy-Authorization, WWW-Authenticate, Proxy-Authenticate, Set-Cookie, Cookie (authentication)
//   - Content-Encoding, Content-Type, Content-Range (content metadata)
//   - Expect, Max-Forwards, Range, If-* conditionals, TE (request controls and conditionals)
// This is a conservative list for safety and correctness.
constexpr bool IsForbiddenTrailerHeader(LowerAsciiKey trailerName) noexcept {
  static constexpr LowerAsciiKey kForbiddenTrailers[]{
      "authorization",
      "cache-control",
      "connection",
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
      "if-range",
      "if-unmodified-since",
      "max-forwards",
      "pragma",
      "proxy-authenticate",
      "proxy-authorization",
      "range",
      "set-cookie",
      "te",
      "trailer",
      "transfer-encoding",
      "upgrade",
      "vary",
      "www-authenticate",
  };

  // Compared as std::string_view: the lengths first, inline (the defaulted LowerAsciiKey comparison may not be).
  const std::string_view name = trailerName;
  return std::ranges::any_of(kForbiddenTrailers, [name](LowerAsciiKey candidate) { return name == candidate.get(); });
}

// RFC 9113 §8.2.2: a connection-specific field makes an HTTP/2 message malformed. TE is the exception when its value is
// "trailers", case-insensitive like any literal of the field grammar (RFC 9110 §10.1.4). 'name' must be in lower case.
// Plain comparison loop to keep <immintrin.h> out of the HPACK decoder. Most names do not have the length of any of
// these fields.
constexpr bool IsConnectionSpecificField(std::string_view name, std::string_view value) noexcept {
  static constexpr std::string_view kProxyConnection = "proxy-connection";
  static constexpr std::string_view kTrailers = "trailers";
  switch (name.size()) {
    case http::TE.size():
      return name == http::TE &&
             !std::ranges::equal(value, kTrailers, [](char lhs, char rhs) { return tolower(lhs) == rhs; });
    case http::Upgrade.size():
      return name == http::Upgrade;
    case http::Connection.size():  // also the size of keep-alive
      return name == http::Connection || name == http::keepalive;
    case kProxyConnection.size():
      return name == kProxyConnection;
    case http::TransferEncoding.size():
      return name == http::TransferEncoding;
    default:
      return false;
  }
}

}  // namespace aeronet::http