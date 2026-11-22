#include "cors.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-value-reverse-tokens-iterator.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "web-url.hpp"

namespace aeronet::internal {

namespace {

// The forbidden request-header names of the Fetch Standard, lower-case, but for the prefixes and the method overrides.
constexpr std::string_view kForbiddenRequestHeaders[]{
    "accept-charset",
    "accept-encoding",
    "access-control-request-headers",
    "access-control-request-method",
    "connection",
    "content-length",
    "cookie",
    "cookie2",
    "date",
    "dnt",
    "expect",
    "host",
    "keep-alive",
    "origin",
    "referer",
    "set-cookie",
    "te",
    "trailer",
    "transfer-encoding",
    "upgrade",
    "via",
};

// The headers that override the method of a request for some servers.
constexpr std::string_view kMethodOverrideHeaders[]{"x-http-method", "x-http-method-override", "x-method-override"};

// The response headers that a script can read from another origin without Access-Control-Expose-Headers.
constexpr std::string_view kSafelistedResponseHeaders[]{
    "cache-control", "content-language", "content-length", "content-type", "expires", "last-modified", "pragma",
};

// The maximum size of the value of a CORS-safelisted request header.
constexpr std::size_t kMaxSafelistedValueSize = 128;

// Fetch "CORS-unsafe request-header byte".
constexpr bool IsCorsUnsafeByte(char ch) noexcept {
  const auto byte = static_cast<unsigned char>(ch);
  if (byte < 0x20U) {
    return byte != '\t';
  }
  return std::string_view("\"():<>?@[\\]{}\x7F").contains(ch);
}

constexpr bool HasCorsUnsafeByte(std::string_view value) noexcept {
  return std::ranges::any_of(value, IsCorsUnsafeByte);
}

// The value of Accept-Language and Content-Language: only these bytes.
constexpr bool IsLanguageByte(char ch) noexcept {
  return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
         std::string_view(" *,-.;=").contains(ch);
}

// Whether the Content-Type 'value' is of a type that an HTML form can send: its essence (before the parameters) is
// application/x-www-form-urlencoded, multipart/form-data or text/plain, case-insensitively.
constexpr bool IsFormContentType(std::string_view value) noexcept {
  const std::string_view essence = TrimOws(value.substr(0, value.find(';')));
  return CaseInsensitiveEqual(essence, "application/x-www-form-urlencoded") ||
         CaseInsensitiveEqual(essence, "multipart/form-data") || CaseInsensitiveEqual(essence, "text/plain");
}

constexpr bool IsDigit(char ch) noexcept { return ch >= '0' && ch <= '9'; }

// Fetch "simple range header value": "bytes=" followed by a start and an optional end, or by "-" and an end... only the
// forms with a start are safelisted: "bytes=N-" and "bytes=N-M" with N <= M.
constexpr bool IsSimpleRange(std::string_view value) noexcept {
  if (!value.starts_with("bytes=")) {
    return false;
  }

  value.remove_prefix(std::string_view("bytes=").size());

  const auto dash = value.find('-');
  if (dash == 0 || dash == std::string_view::npos) {
    return false;
  }

  const std::string_view start = value.substr(0, dash);
  const std::string_view end = value.substr(dash + 1);
  if (!std::ranges::all_of(start, IsDigit) || !std::ranges::all_of(end, IsDigit)) {
    return false;
  }

  // Without leading zeros, a longer number is a larger one.
  const auto significant = [](std::string_view digits) {
    return digits.substr(std::min(digits.find_first_not_of('0'), digits.size()));
  };

  const std::string_view first = significant(start);
  const std::string_view last = significant(end);
  return end.empty() || first.size() < last.size() || (first.size() == last.size() && first <= last);
}

}  // namespace

bool IsForbiddenRequestHeader(std::string_view name, std::string_view value) noexcept {
  if (name.starts_with("proxy-") || name.starts_with("sec-") || std::ranges::contains(kForbiddenRequestHeaders, name)) {
    return true;
  }
  if (!std::ranges::contains(kMethodOverrideHeaders, name)) {
    return false;
  }
  return http::HeaderValueListContainsCI(value, "connect") || http::HeaderValueListContainsCI(value, "trace") ||
         http::HeaderValueListContainsCI(value, "track");
}

bool IsCorsSafelistedRequestHeader(std::string_view name, std::string_view value) noexcept {
  if (value.size() > kMaxSafelistedValueSize) {
    return false;
  }
  if (name == "accept") {
    return !HasCorsUnsafeByte(value);
  }
  if (name == "accept-language" || name == "content-language") {
    return std::ranges::all_of(value, IsLanguageByte);
  }
  if (name == "content-type") {
    return !HasCorsUnsafeByte(value) && IsFormContentType(value);
  }
  if (name == "range") {
    return IsSimpleRange(value);
  }
  return false;
}

bool CorsCheck(const HttpResponse& response, std::string_view origin, bool credentials) noexcept {
  const std::string_view allowOrigin = response.headerValueOrEmpty(http::AccessControlAllowOrigin);
  if (allowOrigin == "*" && !credentials) {
    return true;
  }
  if (allowOrigin != origin) {
    return false;
  }
  return !credentials || response.headerValueOrEmpty(http::AccessControlAllowCredentials) == "true";
}

bool PreflightAllows(const HttpResponse& response, std::string_view origin, bool credentials, http::Method method,
                     std::string_view unsafeHeaderNames) noexcept {
  if (response.status() < http::StatusCodeOK || response.status() >= 300 || !CorsCheck(response, origin, credentials)) {
    return false;
  }
  const std::string_view allowMethods = response.headerValueOrEmpty(http::AccessControlAllowMethods);
  if (!IsCorsSafelistedMethod(method) && !http::HeaderValueListContains(allowMethods, http::MethodToStr(method)) &&
      (credentials || !http::HeaderValueListContains(allowMethods, "*"))) {
    return false;
  }
  const std::string_view allowHeaders = response.headerValueOrEmpty(http::AccessControlAllowHeaders);
  const bool wildcard = !credentials && http::HeaderValueListContains(allowHeaders, "*");
  for (http::HeaderValueReverseTokensIterator names(unsafeHeaderNames); names.hasNext();) {
    const std::string_view name = names.next();
    if (!http::HeaderValueListContainsCI(allowHeaders, name) && (!wildcard || name == http::Authorization)) {
      return false;
    }
  }
  return true;
}

bool IsExposedResponseHeader(std::string_view name, std::string_view exposeHeaders, bool credentials) noexcept {
  if (IsForbiddenResponseHeader(name)) {
    return false;
  }
  return std::ranges::contains(kSafelistedResponseHeaders, name) ||
         http::HeaderValueListContainsCI(exposeHeaders, name) ||
         (!credentials && http::HeaderValueListContains(exposeHeaders, "*"));
}

std::string_view ReferrerOf(std::string_view documentUrl, std::string_view documentOrigin,
                            std::string_view targetUrl) noexcept {
  if (documentUrl.starts_with("https:") && !targetUrl.starts_with("https:")) {
    return {};
  }
  if (IsOfOrigin(targetUrl, documentOrigin)) {
    return WithoutFragment(documentUrl);
  }
  // The path of a serialized http or https URL starts with '/', right after its origin.
  return documentUrl.substr(0, documentOrigin.size() + 1U);
}

}  // namespace aeronet::internal
