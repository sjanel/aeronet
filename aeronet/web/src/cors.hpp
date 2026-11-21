#pragma once

#include <cstdint>
#include <string_view>

#include "aeronet/http-method.hpp"

namespace aeronet {

class HttpResponse;

namespace internal {

// The rules of the Fetch Standard for the requests of the scripts of a page (fetch(), XMLHttpRequest): the headers
// that scripts cannot set, the CORS protocol that decides whether a script can read a response of another origin, and
// the Referer of the requests. The URLs are serialized http or https URLs, and the origins their prefixes
// ("https://example.com:8443", see DocumentUrl).

// The mode of a request (Fetch "request mode").
enum class RequestMode : uint8_t {
  Cors,        // a request to another origin needs the consent of its server (fetch() by default, XMLHttpRequest)
  NoCors,      // a request to another origin gets an opaque response, unreadable (only safelisted methods and headers)
  SameOrigin,  // a request to another origin is a network error
};

// Whether the header 'name' (lower-case) is set by the browser, never by a script (Fetch "forbidden request-header"):
// Host, Cookie, Origin, Referer, Content-Length... and the names starting with "proxy-" or "sec-". 'value' is that of
// the header: the X-HTTP-Method(-Override) headers are only forbidden with the CONNECT, TRACE and TRACK methods.
[[nodiscard]] bool IsForbiddenRequestHeader(std::string_view name, std::string_view value) noexcept;

// Whether the header 'name' (lower-case) of 'value' can be sent to another origin without a preflight (Fetch
// "CORS-safelisted request-header"): Accept, Accept-Language, Content-Language, a Content-Type of a form or of text,
// a simple Range, within 128 bytes.
[[nodiscard]] bool IsCorsSafelistedRequestHeader(std::string_view name, std::string_view value) noexcept;

// Whether 'method' can be sent to another origin without a preflight (Fetch "CORS-safelisted method").
[[nodiscard]] constexpr bool IsCorsSafelistedMethod(http::Method method) noexcept {
  return method == http::Method::GET || method == http::Method::HEAD || method == http::Method::POST;
}

// Whether 'response' lets 'origin' read it (Fetch "CORS check"): its Access-Control-Allow-Origin is 'origin', or "*"
// without 'credentials', and with 'credentials', its Access-Control-Allow-Credentials is "true".
[[nodiscard]] bool CorsCheck(const HttpResponse& response, std::string_view origin, bool credentials) noexcept;

// Whether the response of a preflight lets a request of 'method' with the unsafe headers 'unsafeHeaderNames' (the
// value of its Access-Control-Request-Headers: sorted lower-case names separated by ',') be sent (Fetch "CORS-preflight
// fetch"): a successful status, a passed CORS check, and the method and headers allowed by its
// Access-Control-Allow-Methods and Access-Control-Allow-Headers ("*" is not a wildcard with 'credentials', nor for
// Authorization).
[[nodiscard]] bool PreflightAllows(const HttpResponse& response, std::string_view origin, bool credentials,
                                   http::Method method, std::string_view unsafeHeaderNames) noexcept;

// Whether a script can read the response header 'name' (lower-case) of a response of another origin (Fetch
// "CORS-safelisted response-header name"): Cache-Control, Content-Language, Content-Length, Content-Type, Expires,
// Last-Modified, Pragma, and those listed by its Access-Control-Expose-Headers 'exposeHeaders' ("*" for all, but not
// with 'credentials'). Set-Cookie is never readable (see IsForbiddenResponseHeader()).
[[nodiscard]] bool IsExposedResponseHeader(std::string_view name, std::string_view exposeHeaders,
                                           bool credentials) noexcept;

// Whether a script can never read the response header 'name' (lower-case): Set-Cookie and Set-Cookie2.
[[nodiscard]] constexpr bool IsForbiddenResponseHeader(std::string_view name) noexcept {
  return name == "set-cookie" || name == "set-cookie2";
}

// The Referer of a request of a document of 'documentUrl' (of origin 'documentOrigin') to 'targetUrl', with the default
// referrer policy of the browsers, "strict-origin-when-cross-origin": the URL of the document without its fragment
// for its own origin, its origin followed by '/' for another one, none (empty) from https to http. A view of
// 'documentUrl'.
[[nodiscard]] std::string_view ReferrerOf(std::string_view documentUrl, std::string_view documentOrigin,
                                          std::string_view targetUrl) noexcept;

}  // namespace internal

}  // namespace aeronet
