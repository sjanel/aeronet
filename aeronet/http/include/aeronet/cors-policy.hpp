#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "dynamic-concatenated-strings.hpp"

namespace aeronet {

// Policy object responsible for evaluating CORS requests and emitting the relevant headers.
class CorsPolicy {
 public:
  enum class ApplyStatus : std::uint8_t { NotCors, Applied, OriginDenied };

  struct PreflightResult {
    enum class Status : std::uint8_t { NotPreflight, Allowed, OriginDenied, MethodDenied, HeadersDenied };

    Status status{Status::NotPreflight};
    HttpResponse response{http::StatusCodeNoContent};
  };

  // Default constructor: allows any origin, credentials disabled, simple methods (GET, HEAD, POST).
  CorsPolicy();

  // Allow all origins (wildcard). When credentials are enabled the helper mirrors the request origin.
  CorsPolicy& allowAnyOrigin();

  // Add a single origin to the allow-list (case-insensitive match).
  CorsPolicy& allowOrigin(std::string_view origin);

  // Enable/disable Access-Control-Allow-Credentials emission.
  CorsPolicy& allowCredentials(bool enable);

  // Override the method allow-list used during preflight checks (defaults to GET, HEAD, POST).
  CorsPolicy& allowMethods(http::MethodBmp methods);

  // Allow any request header (Access-Control-Allow-Headers: *).
  CorsPolicy& allowAnyRequestHeaders();

  // Add the provided request header to the allowed list (tokens are case-insensitive).
  CorsPolicy& allowRequestHeader(std::string_view header);

  // Expose additional response header to the browser.
  CorsPolicy& exposeHeader(std::string_view header);

  // Set Access-Control-Max-Age for preflight responses.
  CorsPolicy& maxAge(std::chrono::seconds maxAge);

  // Emit Access-Control-Allow-Private-Network on accepted preflight requests.
  CorsPolicy& allowPrivateNetwork(bool enable);

  // Apply CORS headers to a normal (non-preflight) response if the request is a CORS request.
  [[nodiscard]] ApplyStatus applyToResponse(const HttpRequest& request, HttpResponse& response) const;

  // Handle a preflight CORS request and produce the appropriate response.
  [[nodiscard]] PreflightResult handlePreflight(const HttpRequest& request) const;

 private:
  enum class OriginMode : std::uint8_t { Any, Enumerated };

  [[nodiscard]] static bool isPreflightRequest(const HttpRequest& request) noexcept;

  [[nodiscard]] bool originAllowed(std::string_view origin) const noexcept;

  [[nodiscard]] bool methodAllowed(std::string_view methodToken) const noexcept;

  [[nodiscard]] bool requestHeadersAllowed(std::string_view headerList) const;

  void applyResponseHeaders(HttpResponse& response, std::string_view origin) const;

  static constexpr const char kHeaderValueSep[] = ", ";
  static constexpr char kNullCharSep[] = {'\0'};

  DynamicConcatenatedStrings<kNullCharSep, true> _allowedOrigins;
  OriginMode _originMode{OriginMode::Any};
  bool _allowCredentials{false};
  bool _allowPrivateNetwork{false};
  bool _allowAnyRequestHeaders{false};
  http::MethodBmp _allowedMethods{0};
  DynamicConcatenatedStrings<kHeaderValueSep, true> _allowedRequestHeaders;
  DynamicConcatenatedStrings<kHeaderValueSep, true> _exposedHeaders;
  std::chrono::seconds _maxAge{-1};
};

}  // namespace aeronet
