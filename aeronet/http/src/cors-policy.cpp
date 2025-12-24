#include "aeronet/cors-policy.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "aeronet/fixedcapacityvector.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "http-method-parse.hpp"

namespace aeronet {
namespace {

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
  _allowedRequestHeaders.append("*");
  return *this;
}

CorsPolicy& CorsPolicy::allowRequestHeader(std::string_view header) {
  _active = true;
  header = TrimOws(header);
  if (!header.empty() && !_allowedRequestHeaders.contains(header)) {
    _allowedRequestHeaders.append(header);
  }
  return *this;
}

CorsPolicy& CorsPolicy::exposeHeader(std::string_view header) {
  _active = true;
  header = TrimOws(header);
  if (!header.empty() && !_exposedHeaders.contains(header)) {
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

CorsPolicy::ApplyStatus CorsPolicy::applyToResponse(const HttpRequest& request, HttpResponse& response) const {
  if (!_active || isPreflightRequest(request)) {
    return ApplyStatus::NotCors;
  }
  const auto origin = request.headerValueOrEmpty(http::Origin);
  if (origin.empty()) {
    return ApplyStatus::NotCors;
  }
  if (!originAllowed(origin)) {
    response.status(http::StatusCodeForbidden, http::ReasonForbidden);
    response.body(http::ReasonForbidden);
    return ApplyStatus::OriginDenied;
  }
  applyResponseHeaders(response, origin);
  return ApplyStatus::Applied;
}

CorsPolicy::PreflightResult CorsPolicy::handlePreflight(const HttpRequest& request,
                                                        http::MethodBmp routeMethods) const {
  PreflightResult result;
  if (!_active || !isPreflightRequest(request)) {
    return result;
  }

  const auto origin = request.headerValueOrEmpty(http::Origin);
  if (!originAllowed(origin)) {
    result.status = PreflightResult::Status::OriginDenied;
    return result;
  }

  const auto effectiveMethods = effectiveAllowedMethods(routeMethods);

  const std::string_view methodStr = request.headerValueOrEmpty(http::AccessControlRequestMethod);
  if (!methodAllowed(methodStr, effectiveMethods)) {
    result.status = PreflightResult::Status::MethodDenied;
    return result;
  }

  // Distinguish between header absence and an explicitly-present empty value.
  // If the header is present but trims to empty, treat it as an empty list (i.e. no requested headers).
  // Only validate/deny when there are actual requested header tokens.
  std::string_view requestedHeaders;
  bool requestedHeadersPresent = false;
  if (const auto requestedHeadersOpt = request.headerValue(http::AccessControlRequestHeaders); requestedHeadersOpt) {
    requestedHeadersPresent = true;
    requestedHeaders = TrimOws(*requestedHeadersOpt);
    if (!requestedHeaders.empty()) {
      if (!requestHeadersAllowed(requestedHeaders)) {
        result.status = PreflightResult::Status::HeadersDenied;
        return result;
      }
    }
  }

  auto& response = result.response;
  applyResponseHeaders(response, origin);

  FixedCapacityVector<char, http::kAllMethodsStrLen + static_cast<uint32_t>((http::kNbMethods - 1U) * 2U)> acamValues;
  for (http::MethodIdx idx = 0; idx < http::kNbMethods; ++idx) {
    const auto method = http::MethodFromIdx(idx);
    if (http::IsMethodSet(effectiveMethods, method)) {
      if (!acamValues.empty()) {
        static constexpr std::string_view kCommaSep = ", ";
        acamValues.append_range(kCommaSep);
      }
      acamValues.append_range(http::MethodToStr(method));
    }
  }
  response.header(http::AccessControlAllowMethods, std::string_view(acamValues));

  // Only send Access-Control-Allow-Headers when the server allows any header (`*`),
  // or when the client explicitly requested non-empty headers and they passed validation.
  if (_allowedRequestHeaders.fullString() == "*") {
    response.header(http::AccessControlAllowHeaders, "*");
  } else if (requestedHeadersPresent && !requestedHeaders.empty()) {
    // At this point, requestHeadersAllowed() has already been checked for non-empty lists,
    // so it's safe to emit the server's allowed list (which must be non-empty) when present.
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

bool CorsPolicy::isPreflightRequest(const HttpRequest& request) noexcept {
  if (request.method() != http::Method::OPTIONS) {
    return false;
  }
  const auto origin = request.headerValueOrEmpty(http::Origin);
  if (origin.empty()) {
    return false;
  }
  return request.headerValue(http::AccessControlRequestMethod).has_value();
}

bool CorsPolicy::originAllowed(std::string_view origin) const noexcept {
  if (_originMode == OriginMode::Any) {
    return true;
  }
  return _allowedOrigins.containsCI(origin);
}

bool CorsPolicy::methodAllowed(std::string_view methodToken, http::MethodBmp routeMethods) const noexcept {
  const auto effectiveMask = effectiveAllowedMethods(routeMethods);
  if (effectiveMask == 0) {
    return false;
  }
  if (const auto method = http::MethodStrToOptEnum(methodToken); method.has_value()) {
    return http::IsMethodSet(effectiveMask, *method);
  }
  return false;
}

bool CorsPolicy::requestHeadersAllowed(std::string_view headerList) const {
  if (_allowedRequestHeaders.fullString() == "*") {
    return true;
  }
  if (_allowedRequestHeaders.empty()) {
    return false;
  }
  do {
    auto token = NextCsvToken(headerList);
    if (token.empty()) {
      continue;
    }
    if (!_allowedRequestHeaders.contains(token)) {
      return false;
    }
  } while (!headerList.empty());
  return true;
}

void CorsPolicy::applyResponseHeaders(HttpResponse& response, std::string_view origin) const {
  const bool mirrorOrigin = _originMode == OriginMode::Enumerated || _allowCredentials;
  if (mirrorOrigin) {
    response.header(http::AccessControlAllowOrigin, origin);
    const auto optVary = response.headerValue(http::Vary);
    if (optVary) {
      if (!ListContainsToken(*optVary, http::Origin)) {
        response.appendHeaderValue(http::Vary, http::Origin);
      }
    } else {
      response.addHeader(http::Vary, http::Origin);
    }
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

http::MethodBmp CorsPolicy::effectiveAllowedMethods(http::MethodBmp routeMethods) const noexcept {
  // if (_allowedMethods == 0 || routeMethods == 0) {
  //   return 0;
  // }
  // if (routeMethods == kAllMethodsMask) {
  //   return _allowedMethods;
  // }
  return _allowedMethods & routeMethods;
}

}  // namespace aeronet
