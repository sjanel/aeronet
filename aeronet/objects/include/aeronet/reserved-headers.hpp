#pragma once

#include <algorithm>
#include <string_view>

#include "aeronet/string-equal-ignore-case.hpp"

namespace aeronet::http {

// Centralized rule for HTTP response headers the user may not set directly (normal or streaming path).
// These are either automatically emitted (Date, Content-Type, Content-Length, Connection, Transfer-Encoding) or
// would create ambiguous / unsupported semantics if user-supplied before dedicated feature support
// (Trailer, Upgrade, TE). Keeping this here allows future optimization of storage layout without
// scattering the logic.
// You can use 'static_assert' to make sure at compilation time that the header you are about to insert is not
// reserved. The list of reserved headers is unlikely to change in the future, but they are mostly technical /
// framework headers that aeronet manages internally and probably not very interesting for the client.
// Example:
//     static_assert(!aeronet::http::IsReservedResponseHeader("X-My-Header")); // OK
//     static_assert(!aeronet::http::IsReservedResponseHeader("Content-Length")); // Not OK
constexpr bool IsReservedResponseHeader(std::string_view name) noexcept {
  static constexpr std::string_view kHeaders[] = {"connection", "content-length", "content-type",      "date",
                                                  "te",         "trailer",        "transfer-encoding", "upgrade"};
  return std::ranges::any_of(kHeaders,
                             [name](std::string_view candidate) { return CaseInsensitiveEqual(name, candidate); });
}

// Same as IsReservedResponseHeader but for request headers.
constexpr bool IsReservedOrForbiddenRequestHeader(std::string_view name) noexcept {
  static constexpr std::string_view kHeaders[] = {"content-length", "content-type",      "expect", "host", "te",
                                                  "trailer",        "transfer-encoding", "upgrade"};
  return std::ranges::any_of(kHeaders,
                             [name](std::string_view candidate) { return CaseInsensitiveEqual(name, candidate); });
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
constexpr bool IsForbiddenTrailerHeader(std::string_view name) noexcept {
  static constexpr std::string_view kForbiddenOrderedTrailersLowercase[] = {"authorization",
                                                                            "cache-control",
                                                                            "content-encoding",
                                                                            "content-length",
                                                                            "content-range",
                                                                            "content-type",
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
                                                                            "vary"};
  return std::ranges::any_of(kForbiddenOrderedTrailersLowercase,
                             [name](std::string_view candidate) { return CaseInsensitiveEqual(name, candidate); });
}

}  // namespace aeronet::http