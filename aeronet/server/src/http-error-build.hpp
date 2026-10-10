#pragma once

#include <string_view>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

RawChars BuildSimpleError(http::StatusCode status, const ConcatenatedHeaders& globalHeaders, std::string_view body,
                          const char* cachedDateHeader);

// Build the '200 OK' response establishing a CONNECT tunnel. As a 2xx response to CONNECT, it has neither content nor
// Content-Length (RFC 9110 §9.3.6): the tunneled bytes follow its head.
RawChars BuildTunnelEstablished(const ConcatenatedHeaders& globalHeaders, const char* cachedDateHeader);

}  // namespace aeronet
