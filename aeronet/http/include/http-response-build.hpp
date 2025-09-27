#pragma once

#include <string_view>

#include "aeronet/http-response.hpp"
#include "raw-chars.hpp"

namespace aeronet::http {

// Build an HTTP/1.1 response head (status line + basic headers + final CRLFCRLF).
// bodySize allows specifying the length that would be sent (e.g. for HEAD requests
// where the body itself is not transmitted). The date must be a preformatted RFC 7231
// timestamp string. keepAlive controls the Connection header value.
RawChars buildHead(HttpResponse &resp, std::string_view httpVersion, std::string_view date, bool keepAlive);

}  // namespace aeronet::http