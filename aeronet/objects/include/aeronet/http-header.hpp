#pragma once

#include <string>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/string-equal-ignore-case.hpp"

namespace aeronet::http {

struct Header {
  bool operator==(const Header &) const noexcept = default;

  std::string name;
  std::string value;
};

struct HeaderView {
  std::string_view name;
  std::string_view value;
};

// RFC 7230 §3.2: Header field values can be preceded and followed by optional whitespace (OWS).
// OWS is defined as zero or more spaces (SP) or horizontal tabs (HTAB).
constexpr bool IsHeaderWhitespace(char ch) noexcept { return ch == ' ' || ch == '\t'; }

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
  return CaseInsensitiveEqual(name, Connection) || CaseInsensitiveEqual(name, Date) ||
         CaseInsensitiveEqual(name, ContentLength) || CaseInsensitiveEqual(name, TransferEncoding) ||
         CaseInsensitiveEqual(name, Trailer) || CaseInsensitiveEqual(name, Upgrade) || CaseInsensitiveEqual(name, TE);
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
  return CaseInsensitiveEqual(name, TransferEncoding) || CaseInsensitiveEqual(name, ContentLength) ||
         CaseInsensitiveEqual(name, Host) || CaseInsensitiveEqual(name, Trailer) ||
         CaseInsensitiveEqual(name, "Cache-Control") || CaseInsensitiveEqual(name, "Expires") ||
         CaseInsensitiveEqual(name, "Pragma") || CaseInsensitiveEqual(name, "Vary") ||
         CaseInsensitiveEqual(name, "Authorization") || CaseInsensitiveEqual(name, "Set-Cookie") ||
         CaseInsensitiveEqual(name, "Cookie") || CaseInsensitiveEqual(name, "Content-Encoding") ||
         CaseInsensitiveEqual(name, "Content-Type") || CaseInsensitiveEqual(name, "Content-Range") ||
         CaseInsensitiveEqual(name, "Expect") || CaseInsensitiveEqual(name, "Range") || CaseInsensitiveEqual(name, TE);
}

}  // namespace aeronet::http