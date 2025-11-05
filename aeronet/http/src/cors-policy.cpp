#include "aeronet/cors-policy.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "fixedcapacityvector.hpp"
#include "raw-chars.hpp"
#include "string-equal-ignore-case.hpp"

namespace aeronet {
namespace {

[[nodiscard]] std::string_view trimView(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

[[nodiscard]] std::string_view nextCsvToken(std::string_view& csv) {
  const auto commaPos = csv.find(',');
  std::string_view token = commaPos == std::string_view::npos ? csv : csv.substr(0, commaPos);
  if (commaPos == std::string_view::npos) {
    csv = {};
  } else {
    csv.remove_prefix(commaPos + 1);
  }
  return trimView(token);
}

[[nodiscard]] bool listContainsToken(std::string_view list, std::string_view token) {
  std::string_view remaining = list;
  while (!remaining.empty()) {
    auto part = nextCsvToken(remaining);
    if (!part.empty() && CaseInsensitiveEqual(part, token)) {
      return true;
    }
  }
  return false;
}

constexpr http::MethodBmp kAllMethodsMask = static_cast<http::MethodBmp>((1U << http::kNbMethods) - 1U);

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

  origin = trimView(origin);
  if (!origin.empty() && !_allowedOrigins.contains(origin)) {
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
  header = trimView(header);
  if (!header.empty() && !_allowedRequestHeaders.contains(header)) {
    _allowedRequestHeaders.append(header);
  }
  return *this;
}

CorsPolicy& CorsPolicy::exposeHeader(std::string_view header) {
  _active = true;
  header = trimView(header);
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
    response.statusCode(http::StatusCodeForbidden, http::ReasonForbidden);
    response.contentType(http::ContentTypeTextPlain).body(http::ReasonForbidden);
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
  if (origin.empty() || !originAllowed(origin)) {
    result.status = PreflightResult::Status::OriginDenied;
    return result;
  }

  const auto effectiveMethods = effectiveAllowedMethods(routeMethods);

  const auto methodOpt = request.headerValue(http::AccessControlRequestMethod);
  if (!methodOpt || !methodAllowed(*methodOpt, effectiveMethods)) {
    result.status = PreflightResult::Status::MethodDenied;
    return result;
  }

  std::string_view requestedHeaders = request.headerValueOrEmpty(http::AccessControlRequestHeaders);
  if (!requestedHeaders.empty() && !requestHeadersAllowed(requestedHeaders)) {
    result.status = PreflightResult::Status::HeadersDenied;
    return result;
  }

  auto& response = result.response;
  applyResponseHeaders(response, origin);

  if (effectiveMethods != 0) {
    FixedCapacityVector<char, http::kAllMethodsStrLen + static_cast<uint32_t>((http::kNbMethods - 1U) * 2U)> value;
    for (http::MethodIdx idx = 0; idx < http::kNbMethods; ++idx) {
      const auto method = http::fromMethodIdx(idx);
      if (http::isMethodSet(effectiveMethods, method)) {
        if (!value.empty()) {
          value.push_back(',');
          value.push_back(' ');
        }
        value.append_range(http::toMethodStr(method));
      }
    }
    response.customHeader(http::AccessControlAllowMethods, std::string_view(value));
  }

  if (!_allowedRequestHeaders.empty()) {
    response.customHeader(http::AccessControlAllowHeaders, _allowedRequestHeaders.fullString());
  } else if (!requestedHeaders.empty()) {
    response.customHeader(http::AccessControlAllowHeaders, requestedHeaders);
  }

  if (_allowPrivateNetwork) {
    response.customHeader(http::AccessControlAllowPrivateNetwork, "true");
  }

  if (_maxAge.count() >= 0) {
    response.customHeader(http::AccessControlMaxAge, static_cast<std::uint64_t>(_maxAge.count()));
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
  return _allowedOrigins.contains(origin);
}

bool CorsPolicy::methodAllowed(std::string_view methodToken, http::MethodBmp routeMethods) const noexcept {
  const auto effectiveMask = effectiveAllowedMethods(routeMethods);
  if (effectiveMask == 0) {
    return false;
  }
  if (const auto method = http::toMethodEnum(methodToken); method.has_value()) {
    return http::isMethodSet(effectiveMask, *method);
  }
  return false;
}

bool CorsPolicy::requestHeadersAllowed(std::string_view headerList) const {
  if (_allowedRequestHeaders.fullString() == "*") {
    return true;
  }
  auto remaining = trimView(headerList);
  if (remaining.empty()) {
    return _allowedRequestHeaders.empty();
  }
  if (_allowedRequestHeaders.empty()) {
    return false;
  }
  while (!remaining.empty()) {
    auto token = nextCsvToken(remaining);
    if (token.empty()) {
      continue;
    }
    if (!_allowedRequestHeaders.contains(token)) {
      return false;
    }
  }
  return true;
}

void CorsPolicy::applyResponseHeaders(HttpResponse& response, std::string_view origin) const {
  const bool mirrorOrigin = _originMode == OriginMode::Enumerated || _allowCredentials;
  if (mirrorOrigin) {
    response.customHeader(http::AccessControlAllowOrigin, origin);
    auto existing = response.headerValueOrEmpty(http::Vary);
    if (existing.empty()) {
      response.addCustomHeader(http::Vary, http::Origin);
    } else if (!listContainsToken(existing, http::Origin)) {
      static constexpr std::string_view kAppendedOrigin = ", Origin";
      SmallRawChars combined(static_cast<uint32_t>(existing.size()) + kAppendedOrigin.size());
      combined.unchecked_append(existing);
      combined.unchecked_append(kAppendedOrigin);
      response.customHeader(http::Vary, std::move(combined));
    }
  } else {
    response.customHeader(http::AccessControlAllowOrigin, "*");
  }

  if (_allowCredentials) {
    response.customHeader(http::AccessControlAllowCredentials, "true");
  }

  if (!_exposedHeaders.empty()) {
    response.customHeader(http::AccessControlExposeHeaders, _exposedHeaders.fullString());
  }
}

http::MethodBmp CorsPolicy::effectiveAllowedMethods(http::MethodBmp routeMethods) const noexcept {
  if (_allowedMethods == 0 || routeMethods == 0) {
    return 0;
  }
  if (routeMethods == kAllMethodsMask) {
    return _allowedMethods;
  }
  return _allowedMethods & routeMethods;
}

}  // namespace aeronet
