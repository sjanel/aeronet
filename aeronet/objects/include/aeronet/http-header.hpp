#pragma once

#include <string>
#include <utility>

#include "http-constants.hpp"
#include "string-equal-ignore-case.hpp"

namespace aeronet::http {

using Header = std::pair<std::string, std::string>;

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

}  // namespace aeronet::http