#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

// Copies to 'echo' the head of the HTTP/1 request starting 'input' if it is a TRACE request: the message echoed by its
// response (RFC 9110 §9.3.8), as received. It must be copied before the head is parsed, which modifies it in place. The
// header fields likely to contain sensitive data are left out: the credentials (Authorization, Proxy-Authorization) and
// the cookies (Cookie).
// Returns false if the head of the TRACE request is not complete yet: it must not be parsed before it is copied by a
// next call. 'echo' is left empty for another method, and for a head longer than 'maxHeaderBytes' or holding a bare LF,
// rejected anyway.
[[nodiscard]] bool CopyTraceEcho(std::string_view input, std::size_t maxHeaderBytes, RawChars32& echo);

}  // namespace aeronet
