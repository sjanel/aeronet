#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/http-status-code.hpp"

namespace aeronet::http {

// NOTE ON CASE SENSITIVITY
// ------------------------
// HTTP header field names are case-insensitive per RFC 7230. Comparison in parsing code
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

// Standard Header Field Names
// They are in lowercase to comply with HTTP/2 header field name rules (RFC 9113)
inline constexpr std::string_view Connection = "connection";
inline constexpr std::string_view TransferEncoding = "transfer-encoding";
inline constexpr std::string_view ContentLength = "content-length";
inline constexpr std::string_view ContentType = "content-type";
inline constexpr std::string_view CacheControl = "cache-control";
inline constexpr std::string_view ContentDisposition = "content-disposition";
inline constexpr std::string_view ContentEncoding = "content-encoding";
inline constexpr std::string_view AcceptEncoding = "accept-encoding";
inline constexpr std::string_view UserAgent = "user-agent";
inline constexpr std::string_view RetryAfter = "retry-after";
inline constexpr std::string_view TE = "te";
inline constexpr std::string_view Trailer = "trailer";
inline constexpr std::string_view Upgrade = "upgrade";
inline constexpr std::string_view Expect = "expect";
inline constexpr std::string_view Host = "host";
inline constexpr std::string_view Date = "date";  // only used for writing (server side)
inline constexpr std::string_view Location = "location";
inline constexpr std::string_view Vary = "vary";
inline constexpr std::string_view Origin = "origin";
inline constexpr std::string_view Allow = "allow";
inline constexpr std::string_view AcceptRanges = "accept-ranges";
inline constexpr std::string_view ContentRange = "content-range";
inline constexpr std::string_view ETag = "etag";
inline constexpr std::string_view LastModified = "last-modified";
inline constexpr std::string_view Range = "range";
inline constexpr std::string_view IfRange = "if-range";
inline constexpr std::string_view IfModifiedSince = "if-modified-since";
inline constexpr std::string_view IfUnmodifiedSince = "if-unmodified-since";
inline constexpr std::string_view IfNoneMatch = "if-none-match";
inline constexpr std::string_view IfMatch = "if-match";
inline constexpr std::string_view AccessControlAllowOrigin = "access-control-allow-origin";
inline constexpr std::string_view AccessControlAllowCredentials = "access-control-allow-credentials";
inline constexpr std::string_view AccessControlAllowMethods = "access-control-allow-methods";
inline constexpr std::string_view AccessControlAllowHeaders = "access-control-allow-headers";
inline constexpr std::string_view AccessControlExposeHeaders = "access-control-expose-headers";
inline constexpr std::string_view AccessControlMaxAge = "access-control-max-age";
inline constexpr std::string_view AccessControlAllowPrivateNetwork = "access-control-allow-private-network";
inline constexpr std::string_view AccessControlRequestMethod = "access-control-request-method";
inline constexpr std::string_view AccessControlRequestHeaders = "access-control-request-headers";
// Custom Headers for Static File Handling
inline constexpr std::string_view XDirectoryListingTruncated = "x-directory-listing-truncated";

// Special aeronet headers
inline constexpr std::string_view OriginalEncodingHeaderName = "x-aeronet-original-encoding";
inline constexpr std::string_view OriginalEncodedLengthHeaderName = "x-aeronet-original-encoded-length";

#ifdef AERONET_ENABLE_HTTP2
// HTTP2 pseudo-headers
inline constexpr std::string_view PseudoHeaderMethod = ":method";
inline constexpr std::string_view PseudoHeaderScheme = ":scheme";
inline constexpr std::string_view PseudoHeaderAuthority = ":authority";
inline constexpr std::string_view PseudoHeaderPath = ":path";
inline constexpr std::string_view PseudoHeaderStatus = ":status";
#endif

inline constexpr std::string_view HeaderSep = ": ";
inline constexpr std::string_view CRLF = "\r\n";
inline constexpr std::string_view DoubleCRLF = "\r\n\r\n";
inline constexpr std::string_view EndChunk = "0\r\n\r\n";

// Minimal syntactic request-line example (no headers):
//   "GET / HTTP/1.1\r\n"
// The expression below computes the minimal request-line length for a
// one-character target and the chosen method token (here we use `GET`).
// Note: HTTP/1.1 requires a Host header (RFC 7230 §5.4). The bare
// request-line alone (shown above) is valid syntactically but is NOT a
// complete HTTP/1.1 request unless a Host header field is present. We
// therefore expose two compile-time minima:
//  - kHttpReqLineMinLen: minimal request-line length (HTTP/1.0 or 1.1)
inline constexpr std::size_t kHttpReqLineMinLen = GET.size() + 3UL + HTTP11Sv.size() + CRLF.size();

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
inline constexpr std::string_view HTTP11_102_PROCESSING = "HTTP/1.1 102 Processing\r\n\r\n";

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
inline constexpr std::string_view ContentTypeTextCss = "text/css";
inline constexpr std::string_view ContentTypeTextJavascript = "text/javascript";

// The shortest known content type is "text/n3" (length 7), so this is a lower bound on the length of any content type
// value we expect to encounter. This can be used for optimizations in parsing code.
// Source: https://www.iana.org/assignments/media-types/media-types.xhtml
inline constexpr std::size_t ContentTypeMinLen = 7;

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