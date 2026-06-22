#pragma once

#include <cstdint>
#include <string_view>

#include "aeronet/raw-chars.hpp"

namespace aeronet::http {

// Removes a trailing ":port" specifier from a Host header value, returning the bare host.
// Handles bracketed IPv6 literals (e.g. "[::1]:80" -> "[::1]", "[::1]" -> "[::1]") and plain
// hostnames / IPv4 ("example.com:80" -> "example.com"). A value without a port is returned as-is.
[[nodiscard]] std::string_view StripPortFromHost(std::string_view hostHeader) noexcept;

// Writes the absolute-URL authority "https://host[:port]" into 'out' (cleared first).
//   - hostHeader : request Host header value; any embedded port is stripped and replaced by targetPort.
//   - targetPort : HTTPS port to advertise; the standard port 443 is omitted from the URL, any other
//                  value is appended as ":port".
// Returns false (and leaves 'out' empty) when the host is empty (no absolute URL can be built).
// On success the caller appends the path/query via AppendUrlEncodedPath / AppendUrlEncodedQueryParam.
[[nodiscard]] bool AppendHttpsAuthority(RawChars& out, std::string_view hostHeader, uint16_t targetPort);

// Appends a (URL-decoded) request path to 'out', percent-encoding any character that is not safe in a
// URL path component. Path-structural characters ('/', sub-delims, ':' and '@') are preserved; spaces,
// control characters, '?', '#', '%' and other unsafe bytes are percent-encoded so the result is always a
// valid, injection-free URL/header value.
void AppendUrlEncodedPath(RawChars& out, std::string_view decodedPath);

// Appends a single (URL-decoded) query parameter to 'out' as "<lead>key=value", percent-encoding the key and
// value down to the RFC 3986 unreserved set so structural characters ('&', '=', '+', spaces, ...) cannot break
// out of their component. 'leadingChar' should be '?' for the first parameter and '&' for subsequent ones.
void AppendUrlEncodedQueryParam(RawChars& out, char leadingChar, std::string_view decodedKey,
                                std::string_view decodedValue);

}  // namespace aeronet::http
