#pragma once

#include <string_view>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

// Build an error response closing the connection, with a 'text/plain' body. The response to a HEAD request
// (isHeadMethod) has the header fields of the GET one, Content-Length included, without its body (RFC 9110 §9.3.2).
RawChars BuildSimpleError(http::StatusCode status, const ConcatenatedHeaders& globalHeaders, std::string_view body,
                          const char* cachedDateHeader, bool isHeadMethod = false);

// Build the '200 OK' response establishing a CONNECT tunnel. As a 2xx response to CONNECT, it has neither content nor
// Content-Length (RFC 9110 §9.3.6): the tunneled bytes follow its head.
RawChars BuildTunnelEstablished(const ConcatenatedHeaders& globalHeaders, const char* cachedDateHeader);

}  // namespace aeronet
