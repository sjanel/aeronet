#pragma once

#include <string_view>

#include "http-status-code.hpp"

namespace aeronet::http {

// NOTE ON CASE SENSITIVITY
// ------------------------
// HTTP header field names are case-insensitive per RFC 7230. We store them here
// in their conventional canonical form for emission. Comparison in parsing code
// should remain case-insensitive where required. Header values such as the
// tokens below (e.g. "chunked", "keep-alive") are also case-insensitive in the
// protocol; we keep them lowercase to make case-insensitive comparisons cheaper
// (single pass tolower by caller or direct CaseInsensitiveEqual).

// Version
inline constexpr std::string_view HTTP10 = "HTTP/1.0";
inline constexpr std::string_view HTTP11 = "HTTP/1.1";

// Methods
inline constexpr std::string_view HEAD = "HEAD";
inline constexpr std::string_view GET = "GET";
inline constexpr std::string_view POST = "POST";
inline constexpr std::string_view PUT = "PUT";
inline constexpr std::string_view DELETE = "DELETE";

// Standard Header Field Names (as they typically appear in canonical form)
inline constexpr std::string_view Connection = "Connection";
inline constexpr std::string_view ContentLength = "Content-Length";
inline constexpr std::string_view TransferEncoding = "Transfer-Encoding";
inline constexpr std::string_view Expect = "Expect";
inline constexpr std::string_view Host = "Host";
inline constexpr std::string_view Date = "Date";  // only used for writing (server side)
inline constexpr std::string_view ContentType = "Content-Type";

// Common Header Values (lowercase tokens where case-insensitive comparison used)
inline constexpr std::string_view keepalive = "keep-alive";
inline constexpr std::string_view close = "close";
inline constexpr std::string_view chunked = "chunked";
inline constexpr std::string_view h100_continue = "100-continue";  // value of Expect header

// Preformatted interim response line
inline constexpr std::string_view HTTP11_100_CONTINUE = "HTTP/1.1 100 Continue\r\n\r\n";

// Reason Phrases (only those we currently emit explicitly)
inline constexpr std::string_view ReasonBadRequest = "Bad Request";                              // 400
inline constexpr std::string_view ReasonMethodNotAllowed = "Method Not Allowed";                 // 405
inline constexpr std::string_view ReasonPayloadTooLarge = "Payload Too Large";                   // 413
inline constexpr std::string_view ReasonHeadersTooLarge = "Request Header Fields Too Large";     // 431
inline constexpr std::string_view ReasonInternalServerError = "Internal Server Error";           // 500
inline constexpr std::string_view ReasonNotImplemented = "Not Implemented";                      // 501
inline constexpr std::string_view ReasonHTTPVersionNotSupported = "HTTP Version Not Supported";  // 505

inline constexpr std::string_view CRLF = "\r\n";

// Return the canonical reason phrase for a subset of status codes we care about.
// If an unmapped status is provided, returns an empty string_view, letting callers
// decide whether to supply a custom phrase.
constexpr std::string_view reasonPhraseFor(http::StatusCode status) noexcept {
  switch (status) {
    case 400:
      return ReasonBadRequest;
    case 405:
      return ReasonMethodNotAllowed;
    case 413:
      return ReasonPayloadTooLarge;
    case 431:
      return ReasonHeadersTooLarge;
    case 500:
      return ReasonInternalServerError;
    case 501:
      return ReasonNotImplemented;
    case 505:
      return ReasonHTTPVersionNotSupported;
    default:
      return ReasonNotImplemented;
  }
}

}  // namespace aeronet::http