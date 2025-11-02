#include "aeronet/cors-policy.hpp"

#include <utility>

#include "aeronet/http-constants.hpp"
#include "string-equal-ignore-case.hpp"
#include "stringconv.hpp"

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

constexpr http::MethodBmp defaultSimpleMethods() { return http::Method::GET | http::Method::HEAD | http::Method::POST; }

}  // namespace

CorsPolicy::CorsPolicy() : _allowedMethods(defaultSimpleMethods()) {}

CorsPolicy& CorsPolicy::allowAnyOrigin() {
  _originMode = OriginMode::Any;
  _allowedOrigins.clear();
  return *this;
}

CorsPolicy& CorsPolicy::allowOrigin(std::string_view origin) {
  _originMode = OriginMode::Enumerated;
  origin = trimView(origin);
  if (!origin.empty() && !_allowedOrigins.contains(origin)) {
    _allowedOrigins.append(origin);
  }
  return *this;
}

CorsPolicy& CorsPolicy::allowCredentials(bool enable) {
  _allowCredentials = enable;
  return *this;
}

CorsPolicy& CorsPolicy::allowMethods(http::MethodBmp methods) {
  _allowedMethods = methods;
  return *this;
}

CorsPolicy& CorsPolicy::allowAnyRequestHeaders() {
  _allowAnyRequestHeaders = true;
  _allowedRequestHeaders.clear();
  return *this;
}

CorsPolicy& CorsPolicy::allowRequestHeader(std::string_view header) {
  _allowAnyRequestHeaders = false;
  header = trimView(header);
  if (!header.empty() && !_allowedRequestHeaders.contains(header)) {
    _allowedRequestHeaders.append(header);
  }
  return *this;
}

CorsPolicy& CorsPolicy::exposeHeader(std::string_view header) {
  header = trimView(header);
  if (!header.empty() && !_exposedHeaders.contains(header)) {
    _exposedHeaders.append(header);
  }

  return *this;
}

CorsPolicy& CorsPolicy::maxAge(std::chrono::seconds maxAge) {
  _maxAge = maxAge.count() >= 0 ? maxAge : std::chrono::seconds{-1};
  return *this;
}

CorsPolicy& CorsPolicy::allowPrivateNetwork(bool enable) {
  _allowPrivateNetwork = enable;
  return *this;
}

CorsPolicy::ApplyStatus CorsPolicy::applyToResponse(const HttpRequest& request, HttpResponse& response) const {
  if (isPreflightRequest(request)) {
    return ApplyStatus::NotCors;
  }
  const auto origin = request.headerValueOrEmpty(http::Origin);
  if (origin.empty()) {
    return ApplyStatus::NotCors;
  }
  if (!originAllowed(origin)) {
    return ApplyStatus::OriginDenied;
  }
  applyResponseHeaders(response, origin);
  return ApplyStatus::Applied;
}

CorsPolicy::PreflightResult CorsPolicy::handlePreflight(const HttpRequest& request) const {
  PreflightResult result;
  if (!isPreflightRequest(request)) {
    return result;
  }

  const auto origin = request.headerValueOrEmpty(http::Origin);
  if (origin.empty() || !originAllowed(origin)) {
    result.status = PreflightResult::Status::OriginDenied;
    return result;
  }

  const auto methodOpt = request.headerValue(http::AccessControlRequestMethod);
  if (!methodOpt || !methodAllowed(*methodOpt)) {
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

  if (_allowedMethods != 0) {
    SmallRawChars value;
    for (http::MethodIdx idx = 0; idx < http::kNbMethods; ++idx) {
      const auto method = http::fromMethodIdx(idx);
      if (!http::isMethodSet(_allowedMethods, method)) {
        continue;
      }
      if (!value.empty()) {
        value.append(", ");
      }
      value.append(http::toMethodStr(method));
    }
    response.customHeader(http::AccessControlAllowMethods, std::move(value));
  }

  if (_allowAnyRequestHeaders) {
    response.customHeader(http::AccessControlAllowHeaders, "*");
  } else if (!_allowedRequestHeaders.empty()) {
    response.customHeader(http::AccessControlAllowHeaders, _allowedRequestHeaders.fullString());
  } else if (!requestedHeaders.empty()) {
    response.customHeader(http::AccessControlAllowHeaders, trimView(requestedHeaders));
  }

  if (_allowPrivateNetwork) {
    response.customHeader(http::AccessControlAllowPrivateNetwork, "true");
  }

  if (_maxAge.count() >= 0) {
    const auto value = IntegralToCharVector(static_cast<std::uint64_t>(_maxAge.count()));
    response.customHeader(http::AccessControlMaxAge, std::string_view(value.data(), value.size()));
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

bool CorsPolicy::methodAllowed(std::string_view methodToken) const noexcept {
  if (_allowedMethods == 0) {
    return false;
  }
  if (const auto method = http::toMethodEnum(methodToken); method.has_value()) {
    return http::isMethodSet(_allowedMethods, *method);
  }
  return false;
}

bool CorsPolicy::requestHeadersAllowed(std::string_view headerList) const {
  if (_allowAnyRequestHeaders) {
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
      response.addCustomHeader(http::Vary, "Origin");
    } else if (!listContainsToken(existing, "Origin")) {
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

}  // namespace aeronet
