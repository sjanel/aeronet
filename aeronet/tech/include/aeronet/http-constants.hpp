#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/http-status-code.hpp"

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
inline constexpr std::string_view TransferEncoding = "Transfer-Encoding";
inline constexpr std::string_view ContentLength = "Content-Length";
inline constexpr std::string_view ContentType = "Content-Type";
inline constexpr std::string_view ContentDisposition = "Content-Disposition";
inline constexpr std::string_view ContentEncoding = "Content-Encoding";
inline constexpr std::string_view AcceptEncoding = "Accept-Encoding";
inline constexpr std::string_view UserAgent = "User-Agent";
inline constexpr std::string_view RetryAfter = "Retry-After";
inline constexpr std::string_view TE = "TE";
inline constexpr std::string_view Trailer = "Trailer";
inline constexpr std::string_view Upgrade = "Upgrade";
inline constexpr std::string_view Expect = "Expect";
inline constexpr std::string_view Host = "Host";
inline constexpr std::string_view Date = "Date";  // only used for writing (server side)
inline constexpr std::string_view Location = "Location";
inline constexpr std::string_view Vary = "Vary";
inline constexpr std::string_view Origin = "Origin";
inline constexpr std::string_view Allow = "Allow";
inline constexpr std::string_view AcceptRanges = "Accept-Ranges";
inline constexpr std::string_view ContentRange = "Content-Range";
inline constexpr std::string_view ETag = "ETag";
inline constexpr std::string_view LastModified = "Last-Modified";
inline constexpr std::string_view Range = "Range";
inline constexpr std::string_view IfRange = "If-Range";
inline constexpr std::string_view IfModifiedSince = "If-Modified-Since";
inline constexpr std::string_view IfUnmodifiedSince = "If-Unmodified-Since";
inline constexpr std::string_view IfNoneMatch = "If-None-Match";
inline constexpr std::string_view IfMatch = "If-Match";
inline constexpr std::string_view AccessControlAllowOrigin = "Access-Control-Allow-Origin";
inline constexpr std::string_view AccessControlAllowCredentials = "Access-Control-Allow-Credentials";
inline constexpr std::string_view AccessControlAllowMethods = "Access-Control-Allow-Methods";
inline constexpr std::string_view AccessControlAllowHeaders = "Access-Control-Allow-Headers";
inline constexpr std::string_view AccessControlExposeHeaders = "Access-Control-Expose-Headers";
inline constexpr std::string_view AccessControlMaxAge = "Access-Control-Max-Age";
inline constexpr std::string_view AccessControlAllowPrivateNetwork = "Access-Control-Allow-Private-Network";
inline constexpr std::string_view AccessControlRequestMethod = "Access-Control-Request-Method";
inline constexpr std::string_view AccessControlRequestHeaders = "Access-Control-Request-Headers";

// Special aeronet headers
inline constexpr std::string_view OriginalEncodingHeaderName = "X-Aeronet-Original-Encoding";
inline constexpr std::string_view OriginalEncodedLengthHeaderName = "X-Aeronet-Original-Encoded-Length";

inline constexpr std::string_view HeaderSep = ": ";
inline constexpr std::string_view CRLF = "\r\n";
inline constexpr std::string_view DoubleCRLF = "\r\n\r\n";
// Minimal syntactic request-line example (no headers):
//   "GET / HTTP/1.1\r\n"
// The expression below computes the minimal request-line length for a
// one-character target and the chosen method token (here we use `GET`).
// Note: HTTP/1.1 requires a Host header (RFC 7230 §5.4). The bare
// request-line alone (shown above) is valid syntactically but is NOT a
// complete HTTP/1.1 request unless a Host header field is present. We
// therefore expose two compile-time minima:
//  - kHttpReqLineMinLen: minimal request-line length (HTTP/1.0 or 1.1)
//  - kHttpReqHeadersMinLenHttp11: minimal complete HTTP/1.1 request including
//    a one-character Host header (e.g. "Host: h\r\n").
inline constexpr std::size_t kHttpReqLineMinLen = GET.size() + 3UL + HTTP11Sv.size() + CRLF.size();

// Minimal complete HTTP/1.0 request (no Host header required by the spec):
inline constexpr std::size_t kHttpReqHeadersMinLenHttp10 = kHttpReqLineMinLen;

// Minimal complete HTTP/1.1 request: include a minimal Host header value of
// one character. Host header length = "Host" + ": " + value (1 byte) + CRLF
inline constexpr std::size_t kHttpReqHeadersMinLenHttp11 =
    kHttpReqLineMinLen + Host.size() + HeaderSep.size() + 1UL + CRLF.size();

// Compression
inline constexpr std::string_view identity = "identity";
inline constexpr std::string_view gzip = "gzip";
inline constexpr std::string_view deflate = "deflate";
inline constexpr std::string_view zstd = "zstd";  // RFC 8878
inline constexpr std::string_view br = "br";      // RFC 7932 (Brotli)

// Common Header Values (lowercase tokens where case-insensitive comparison used)
inline constexpr std::string_view keepalive = "keep-alive";
inline constexpr std::string_view close = "close";
inline constexpr std::string_view chunked = "chunked";
inline constexpr std::string_view h100_continue = "100-continue";  // value of Expect header

// Preformatted interim response line
inline constexpr std::string_view HTTP11_100_CONTINUE = "HTTP/1.1 100 Continue\r\n\r\n";

// Reason Phrases (only those we currently emit explicitly)
inline constexpr std::string_view ReasonSwitchingProtocols = "Switching Protocols";              // 101
inline constexpr std::string_view ReasonOK = "OK";                                               // 200
inline constexpr std::string_view MovedPermanently = "Moved Permanently";                        // 301
inline constexpr std::string_view ReasonBadRequest = "Bad Request";                              // 400
inline constexpr std::string_view ReasonForbidden = "Forbidden";                                 // 403
inline constexpr std::string_view NotFound = "Not Found";                                        // 404
inline constexpr std::string_view ReasonMethodNotAllowed = "Method Not Allowed";                 // 405
inline constexpr std::string_view ReasonNotAcceptable = "Not Acceptable";                        // 406
inline constexpr std::string_view ReasonPayloadTooLarge = "Payload Too Large";                   // 413
inline constexpr std::string_view ReasonUnsupportedMediaType = "Unsupported Media Type";         // 415
inline constexpr std::string_view ReasonHeadersTooLarge = "Request Header Fields Too Large";     // 431
inline constexpr std::string_view ReasonInternalServerError = "Internal Server Error";           // 500
inline constexpr std::string_view ReasonNotImplemented = "Not Implemented";                      // 501
inline constexpr std::string_view ReasonHTTPVersionNotSupported = "HTTP Version Not Supported";  // 505

// Content type
inline constexpr std::string_view ContentTypeTextPlain = "text/plain";
inline constexpr std::string_view ContentTypeTextHtml = "text/html";
inline constexpr std::string_view ContentTypeApplicationJson = "application/json";
inline constexpr std::string_view ContentTypeApplicationOctetStream = "application/octet-stream";
inline constexpr std::string_view ContentTypeMessageHttp = "message/http";

// Return the canonical reason phrase for a subset of status codes we care about.
constexpr std::string_view ReasonPhraseFor(http::StatusCode status) noexcept {
  switch (status) {
    case StatusCodeSwitchingProtocols:
      return ReasonSwitchingProtocols;
    case StatusCodeOK:
      return ReasonOK;
    case StatusCodeMovedPermanently:
      return MovedPermanently;
    case StatusCodeBadRequest:
      return ReasonBadRequest;
    case StatusCodeForbidden:
      return ReasonForbidden;
    case StatusCodeNotFound:
      return NotFound;
    case StatusCodeMethodNotAllowed:
      return ReasonMethodNotAllowed;
    case StatusCodeNotAcceptable:
      return ReasonNotAcceptable;
    case StatusCodePayloadTooLarge:
      return ReasonPayloadTooLarge;
    case StatusCodeUnsupportedMediaType:
      return ReasonUnsupportedMediaType;
    case StatusCodeRequestHeaderFieldsTooLarge:
      return ReasonHeadersTooLarge;
    case StatusCodeInternalServerError:
      return ReasonInternalServerError;
    case StatusCodeNotImplemented:
      return ReasonNotImplemented;
    case StatusCodeHTTPVersionNotSupported:
      return ReasonHTTPVersionNotSupported;
    default:
      return {};
  }
}

}  // namespace aeronet::http