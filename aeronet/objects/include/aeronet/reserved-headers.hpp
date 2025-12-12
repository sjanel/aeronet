#pragma once

#include <algorithm>
#include <string_view>

#include "aeronet/toupperlower.hpp"

namespace aeronet::http {

// Centralized rule for HTTP response headers the user may not set directly (normal or streaming path).
// These are either automatically emitted (Date, Content-Length, Connection, Transfer-Encoding) or
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
  static constexpr std::string_view kReservedOrderedLowerCaseHeaders[] = {
      "connection", "content-length", "date", "te", "trailer", "transfer-encoding", "upgrade"};
  static_assert(std::ranges::is_sorted(kReservedOrderedLowerCaseHeaders));

  static constexpr auto kMaxLenReserved =
      std::ranges::max_element(kReservedOrderedLowerCaseHeaders, {}, [](std::string_view sv) {
        return sv.size();
      })->size();
  if (name.size() > kMaxLenReserved) {
    return false;
  }

  char lowerCaseName[kMaxLenReserved];
  std::ranges::transform(name, lowerCaseName, [](char ch) { return tolower(ch); });
  return std::ranges::binary_search(kReservedOrderedLowerCaseHeaders, std::string_view{lowerCaseName, name.size()});
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
  static_assert(std::ranges::is_sorted(kForbiddenOrderedTrailersLowercase));
  static constexpr auto kMaxLenReserved =
      std::ranges::max_element(kForbiddenOrderedTrailersLowercase, {}, [](std::string_view sv) {
        return sv.size();
      })->size();
  if (name.size() > kMaxLenReserved) {
    return false;
  }

  char lowerCaseName[kMaxLenReserved];
  std::ranges::transform(name, lowerCaseName, [](char ch) { return tolower(ch); });
  return std::ranges::binary_search(kForbiddenOrderedTrailersLowercase, std::string_view{lowerCaseName, name.size()});
}

}  // namespace aeronet::http