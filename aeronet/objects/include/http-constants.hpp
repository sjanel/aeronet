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
inline constexpr std::string_view HTTP10Sv = "HTTP/1.0";
inline constexpr std::string_view HTTP11Sv = "HTTP/1.1";

// Methods GET, HEAD, POST, PUT, DELETE, CONNECT, OPTIONS, TRACE, PATCH

inline constexpr std::string_view GET = "GET";
inline constexpr std::string_view HEAD = "HEAD";
inline constexpr std::string_view POST = "POST";
inline constexpr std::string_view PUT = "PUT";
inline constexpr std::string_view DELETE = "DELETE";
inline constexpr std::string_view CONNECT = "CONNECT";
inline constexpr std::string_view OPTIONS = "OPTIONS";
inline constexpr std::string_view TRACE = "TRACE";
inline constexpr std::string_view PATCH = "PATCH";

// Standard Header Field Names (as they typically appear in canonical form)
inline constexpr std::string_view Connection = "Connection";
inline constexpr std::string_view ContentLength = "Content-Length";
inline constexpr std::string_view TransferEncoding = "Transfer-Encoding";
inline constexpr std::string_view TE = "TE";
inline constexpr std::string_view Trailer = "Trailer";
inline constexpr std::string_view Upgrade = "Upgrade";
inline constexpr std::string_view Expect = "Expect";
inline constexpr std::string_view Host = "Host";
inline constexpr std::string_view Date = "Date";  // only used for writing (server side)
inline constexpr std::string_view ContentType = "Content-Type";
inline constexpr std::string_view Location = "Location";
inline constexpr std::string_view ContentEncoding = "Content-Encoding";
inline constexpr std::string_view Vary = "Vary";
inline constexpr std::string_view AcceptEncoding = "Accept-Encoding";

inline constexpr std::string_view HeaderSep = ": ";

// Compression
inline constexpr std::string_view identity = "identity";
inline constexpr std::string_view gzip = "gzip";
inline constexpr std::string_view deflate = "deflate";

// Common Header Values (lowercase tokens where case-insensitive comparison used)
inline constexpr std::string_view keepalive = "keep-alive";
inline constexpr std::string_view close = "close";
inline constexpr std::string_view chunked = "chunked";
inline constexpr std::string_view h100_continue = "100-continue";  // value of Expect header

// Preformatted interim response line
inline constexpr std::string_view HTTP11_100_CONTINUE = "HTTP/1.1 100 Continue\r\n\r\n";

// Reason Phrases (only those we currently emit explicitly)
inline constexpr std::string_view ReasonOK = "OK";                                               // 200
inline constexpr std::string_view MovedPermanently = "Moved Permanently";                        // 301
inline constexpr std::string_view ReasonBadRequest = "Bad Request";                              // 400
inline constexpr std::string_view NotFound = "Not Found";                                        // 404
inline constexpr std::string_view ReasonMethodNotAllowed = "Method Not Allowed";                 // 405
inline constexpr std::string_view ReasonNotAcceptable = "Not Acceptable";                        // 406
inline constexpr std::string_view ReasonPayloadTooLarge = "Payload Too Large";                   // 413
inline constexpr std::string_view ReasonHeadersTooLarge = "Request Header Fields Too Large";     // 431
inline constexpr std::string_view ReasonInternalServerError = "Internal Server Error";           // 500
inline constexpr std::string_view ReasonNotImplemented = "Not Implemented";                      // 501
inline constexpr std::string_view ReasonHTTPVersionNotSupported = "HTTP Version Not Supported";  // 505

// Content type
inline constexpr std::string_view ContentTypeTextPlain = "text/plain";

inline constexpr std::string_view CRLF = "\r\n";
inline constexpr std::string_view DoubleCRLF = "\r\n\r\n";

// Return the canonical reason phrase for a subset of status codes we care about.
// If an unmapped status is provided, returns an empty string_view, letting callers
// decide whether to supply a custom phrase.
constexpr std::string_view reasonPhraseFor(http::StatusCode status) noexcept {
  switch (status) {
    case StatusCodeOK:
      return ReasonOK;
    case StatusCodeMovedPermanently:
      return MovedPermanently;
    case StatusCodeBadRequest:
      return ReasonBadRequest;
    case StatusCodeNotFound:
      return NotFound;
    case StatusCodeMethodNotAllowed:
      return ReasonMethodNotAllowed;
    case StatusCodeNotAcceptable:
      return ReasonNotAcceptable;
    case StatusCodePayloadTooLarge:
      return ReasonPayloadTooLarge;
    case StatusCodeRequestHeaderFieldsTooLarge:
      return ReasonHeadersTooLarge;
    case StatusCodeInternalServerError:
      return ReasonInternalServerError;
    case StatusCodeNotImplemented:
      return ReasonNotImplemented;
    case StatusCodeHTTPVersionNotSupported:
      return ReasonHTTPVersionNotSupported;
    default:
      return ReasonNotImplemented;
  }
}

}  // namespace aeronet::http