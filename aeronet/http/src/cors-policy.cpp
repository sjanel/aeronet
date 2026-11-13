#include "aeronet/cors-policy.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"

namespace aeronet {
namespace {

// The serialization of an opaque origin (RFC 6454 §6.2).
constexpr std::string_view kNullOrigin = "null";

// The wildcard of Access-Control-Allow-Headers, and with the name it never covers (Fetch, CORS-preflight fetch).
constexpr std::string_view kAnyRequestHeader = "*";
constexpr std::string_view kAnyRequestHeaderAndAuthorization = "*, authorization";

std::string_view NextCsvToken(std::string_view& csv) {
  const auto commaPos = csv.find(',');
  const std::string_view token = commaPos == std::string_view::npos ? csv : csv.substr(0, commaPos);
  if (commaPos == std::string_view::npos) {
    csv = {};
  } else {
    csv.remove_prefix(commaPos + 1);
  }
  return TrimOws(token);
}

bool ListContainsToken(std::string_view list, std::string_view token) {
  while (!list.empty()) {
    const auto part = NextCsvToken(list);
    if (!part.empty() && CaseInsensitiveEqual(part, token)) {
      return true;
    }
  }
  return false;
}

}  // namespace

CorsPolicy& CorsPolicy::allowAnyOrigin() {
  _active = true;
  _originMode = OriginMode::Any;
  _allowedOrigins.clear();
  return *this;
}

CorsPolicy& CorsPolicy::allowOrigin(std::string_view origin) {
  _active = true;
  _originMode = OriginMode::Enumerated;

  origin = TrimOws(origin);
  if (!origin.empty() && !_allowedOrigins.containsCI(origin)) {
    _allowedOrigins.append(origin);
  }
  return *this;
}

CorsPolicy& CorsPolicy::allowCredentials(bool enable) {
  _active = true;
  _allowCredentials = enable;
  return *this;
}

CorsPolicy& CorsPolicy::allowMethods(http::Method method) {
  _active = true;
  _allowedMethods = static_cast<http::MethodBmp>(method);
  return *this;
}

CorsPolicy& CorsPolicy::allowMethods(http::MethodBmp methods) {
  _active = true;
  _allowedMethods = methods;
  return *this;
}

CorsPolicy& CorsPolicy::allowAnyRequestHeaders() {
  _active = true;
  _allowedRequestHeaders.clear();
  _allowedRequestHeaders.append(kAnyRequestHeader);
  return *this;
}

CorsPolicy& CorsPolicy::allowRequestHeader(std::string_view header) {
  _active = true;
  header = TrimOws(header);
  if (!header.empty() && !_allowedRequestHeaders.containsCI(header)) {
    _allowedRequestHeaders.append(header);
  }
  return *this;
}

CorsPolicy& CorsPolicy::exposeHeader(std::string_view header) {
  _active = true;
  header = TrimOws(header);
  if (!header.empty() && !_exposedHeaders.containsCI(header)) {
    _exposedHeaders.append(header);
  }
  return *this;
}

CorsPolicy& CorsPolicy::maxAge(std::chrono::seconds maxAge) {
  if (maxAge < std::chrono::seconds{0}) {
    throw std::invalid_argument("maxAge must be non-negative");
  }
  _active = true;
  _maxAge = maxAge;
  return *this;
}

CorsPolicy& CorsPolicy::allowPrivateNetwork(bool enable) {
  _active = true;
  _allowPrivateNetwork = enable;
  return *this;
}

bool CorsPolicy::originAllowed(std::string_view origin) const noexcept {
  if (_originMode == OriginMode::Enumerated) {
    return _allowedOrigins.containsCI(origin);
  }
  if (!_allowCredentials) {
    return true;
  }
  // With credentials, the origin is mirrored in the response instead of '*': a received origin with bytes that aeronet
  // does not send (obs-text, or control characters over HTTP/2) is not a serialized origin (RFC 6454 §6.2), it cannot
  // be allowed. Neither is the opaque origin "null" (RFC 6454 §7.3), which any site can make a browser send (sandboxed
  // iframe, data: URL, redirect across origins): granting it a credentialed response would let every site read it.
  // An allow-list may still list it explicitly.
  return http::IsValidHeaderValue(origin) && !CaseInsensitiveEqual(origin, kNullOrigin);
}

CorsPolicy::ApplyStatus CorsPolicy::originStatus(std::string_view origin) const noexcept {
  if (origin.empty()) {
    return ApplyStatus::NotCors;
  }
  return originAllowed(origin) ? ApplyStatus::Applied : ApplyStatus::OriginDenied;
}

CorsPolicy::ApplyStatus CorsPolicy::wouldApply(const HttpRequestView& request) const noexcept {
  if (!_active || isPreflightRequest(request)) {
    return ApplyStatus::NotCors;
  }
  return originStatus(request.headerValueOrEmpty(http::Origin));
}

CorsPolicy::ApplyStatus CorsPolicy::applyToResponse(const HttpRequestView& request, HttpResponse& response) const {
  if (!_active || isPreflightRequest(request)) {
    return ApplyStatus::NotCors;
  }
  const std::string_view origin = request.headerValueOrEmpty(http::Origin);
  const ApplyStatus applyStatus = originStatus(origin);

  if (applyStatus == ApplyStatus::Applied) {
    applyResponseHeaders(response, origin);
  } else if (applyStatus == ApplyStatus::OriginDenied) {
    response.status(http::StatusCodeForbidden);
    response.body("Forbidden by CORS policy");
    response.headerAppendValueIfAbsent(http::Vary, http::Origin);
  } else if (mirrorsOrigin()) {
    // No Origin. Fetch, "CORS protocol and HTTP caches": a cache may serve this response to a later CORS request. Its
    // CORS headers depend on the origin: it varies on Origin.
    response.headerAppendValueIfAbsent(http::Vary, http::Origin);
  } else {
    // No Origin, and the CORS headers do not depend on it ('*'): they are sent, so that the response is the same.
    applyResponseHeaders(response, {});
  }

  return applyStatus;
}

CorsPolicy::PreflightResult CorsPolicy::handlePreflight(const HttpRequestView& request,
                                                        http::MethodBmp routeMethods) const {
  PreflightResult result;

  if (!_active || !isPreflightRequest(request)) {
    return result;
  }

  const std::string_view origin = request.headerValueOrEmpty(http::Origin);
  if (!originAllowed(origin)) {
    result.status = PreflightResult::Status::OriginDenied;
    return result;
  }

  const http::MethodBmp effectiveMethods = effectiveAllowedMethods(routeMethods);

  const std::string_view methodStr = request.headerValueOrEmpty(http::AccessControlRequestMethod);
  if (!methodAllowed(methodStr, effectiveMethods)) {
    result.status = PreflightResult::Status::MethodDenied;
    return result;
  }

  // An absent header, or one that trims to empty, is an empty list (no requested headers): only actual requested header
  // tokens are validated.
  const std::string_view requestedHeaders = TrimOws(request.headerValueOrEmpty(http::AccessControlRequestHeaders));
  if (!requestedHeaders.empty() && !requestHeadersAllowed(requestedHeaders)) {
    result.status = PreflightResult::Status::HeadersDenied;
    return result;
  }

  result.response = request.makeResponse(http::StatusCodeNoContent);

  HttpResponse& response = result.response;

  applyResponseHeaders(response, origin);

  char acamValues[http::kMethodListMaxLen];
  response.header(http::AccessControlAllowMethods,
                  std::string_view(acamValues, http::AppendMethodList(effectiveMethods, acamValues)));

  // Only send Access-Control-Allow-Headers when the server allows any header, or when the client requested headers,
  // which passed validation.
  if (allowsAnyRequestHeader()) {
    // Fetch, CORS-preflight fetch: '*' never covers Authorization, and is a header named '*' for a request with
    // credentials. With credentials, the requested names are listed (they are all tokens, see requestHeadersAllowed()).
    if (_allowCredentials) {
      if (!requestedHeaders.empty()) {
        response.header(http::AccessControlAllowHeaders, requestedHeaders);
      }
    } else if (ListContainsToken(requestedHeaders, http::Authorization)) {
      response.header(http::AccessControlAllowHeaders, kAnyRequestHeaderAndAuthorization);
    } else {
      response.header(http::AccessControlAllowHeaders, kAnyRequestHeader);
    }
  } else if (!requestedHeaders.empty()) {
    // The configured list (non-empty, as requestHeadersAllowed() passed): requested names are never reflected.
    response.header(http::AccessControlAllowHeaders, _allowedRequestHeaders.fullString());
  }

  if (_allowPrivateNetwork) {
    response.header(http::AccessControlAllowPrivateNetwork, "true");
  }

  if (_maxAge.count() >= 0) {
    response.header(http::AccessControlMaxAge, static_cast<std::uint64_t>(_maxAge.count()));
  }

  result.status = PreflightResult::Status::Allowed;
  return result;
}

bool CorsPolicy::isPreflightRequest(const HttpRequestView& request) noexcept {
  if (request.method() != http::Method::OPTIONS) {
    return false;
  }
  const std::string_view origin = request.headerValueOrEmpty(http::Origin);
  if (origin.empty()) {
    return false;
  }
  return request.headerValue(http::AccessControlRequestMethod).has_value();
}

bool CorsPolicy::methodAllowed(std::string_view methodToken, http::MethodBmp routeMethods) const noexcept {
  const http::MethodBmp effectiveMask = effectiveAllowedMethods(routeMethods);
  if (effectiveMask == 0) {
    return false;
  }
  // Unlike header names, methods are case-sensitive (RFC 9110 §9.1), and the Fetch CORS check compares them exactly:
  // a browser sends `patch` as is (only DELETE, GET, HEAD, OPTIONS, POST and PUT are normalized to upper case), and the
  // actual `patch` request would be rejected anyway.
  const http::Method method = http::ParseMethodStr(methodToken);
  if (method != http::kMethodInvalid) {
    return http::IsMethodSet(effectiveMask, method);
  }
  return false;
}

bool CorsPolicy::requestHeadersAllowed(std::string_view headerList) const {
  const bool anyRequestHeader = allowsAnyRequestHeader();
  if (!anyRequestHeader && _allowedRequestHeaders.empty()) {
    return false;
  }
  do {
    std::string_view token = NextCsvToken(headerList);
    if (token.empty()) {
      continue;
    }
    // Any header name, which is a token: the list may then be sent back. Header names are case-insensitive, and
    // browsers send them lower-cased in Access-Control-Request-Headers.
    if (anyRequestHeader ? !http::IsValidHeaderName(token) : !_allowedRequestHeaders.containsCI(token)) {
      return false;
    }
  } while (!headerList.empty());
  return true;
}

void CorsPolicy::applyResponseHeaders(HttpResponse& response, std::string_view origin) const {
  if (mirrorsOrigin()) {
    response.header(http::AccessControlAllowOrigin, origin);
    response.headerAppendValueIfAbsent(http::Vary, http::Origin);
  } else {
    response.header(http::AccessControlAllowOrigin, "*");
  }

  if (_allowCredentials) {
    response.header(http::AccessControlAllowCredentials, "true");
  }

  if (!_exposedHeaders.empty()) {
    response.header(http::AccessControlExposeHeaders, _exposedHeaders.fullString());
  }
}

}  // namespace aeronet
