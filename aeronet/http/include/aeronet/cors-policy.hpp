#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "aeronet/concatenated-header-values.hpp"
#include "aeronet/concatenated-strings.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"

namespace aeronet {

class CorsPolicy;

namespace detail {
struct CorsPolicyData;
void ApplyCorsPolicyData(CorsPolicyData&& data, CorsPolicy& policy);
CorsPolicyData BuildCorsPolicyData(const CorsPolicy& policy);
}  // namespace detail

// Policy object responsible for evaluating CORS requests and emitting the relevant headers.
class CorsPolicy {
 public:
  enum class ApplyStatus : std::uint8_t { NotCors, Applied, OriginDenied };
  enum class Active : std::uint8_t { Off, On };

  struct PreflightResult {
    enum class Status : std::uint8_t { NotPreflight, Allowed, OriginDenied, MethodDenied, HeadersDenied };

    Status status{Status::NotPreflight};
    HttpResponse response{http::StatusCodeNoContent};
  };

  // Default constructor: policy disabled by default. To enable, call the explicit
  // constructor with Active::On.
  CorsPolicy() noexcept = default;

  // Construct and set the active state. When `active == Active::On` the policy is enabled
  // and the other default settings (allow any origin, credentials disabled, simple methods)
  // apply; otherwise the policy is disabled and will be treated as non-CORS.
  explicit CorsPolicy(Active active) : _active(active == Active::On) {}

  // Allow all origins (wildcard). When credentials are enabled the helper mirrors the request origin.
  CorsPolicy& allowAnyOrigin();

  // Add a single origin to the allow-list (case-insensitive match).
  CorsPolicy& allowOrigin(std::string_view origin);

  // Enable/disable Access-Control-Allow-Credentials emission.
  CorsPolicy& allowCredentials(bool enable);

  // Override the method allow-list used during preflight checks (defaults to GET, HEAD, POST).
  CorsPolicy& allowMethods(http::Method method);

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

  // Returns true of the CorsPolicy should be applied.
  // If false, no need to call applyToResponse.
  [[nodiscard]] bool active() const noexcept { return _active; }

  // Determine if CORS headers would be applied to a normal (non-preflight) response.
  [[nodiscard]] ApplyStatus wouldApply(const HttpRequestView& request) const noexcept;

  // Apply CORS headers to a normal (non-preflight) response if the request is a CORS request.
  [[nodiscard]] ApplyStatus applyToResponse(const HttpRequestView& request, HttpResponse& response) const;

  // Handle a preflight CORS request and produce the appropriate response.
  [[nodiscard]] PreflightResult handlePreflight(
      const HttpRequestView& request,
      http::MethodBmp routeMethods = static_cast<http::MethodBmp>((1U << http::kNbMethods) - 1U)) const;

 private:
  enum class OriginMode : std::uint8_t { Any, Enumerated };

  [[nodiscard]] static bool isPreflightRequest(const HttpRequestView& request) noexcept;

  [[nodiscard]] bool originAllowed(std::string_view origin) const noexcept {
    return _originMode == OriginMode::Any || _allowedOrigins.containsCI(origin);
  }

  [[nodiscard]] bool methodAllowed(std::string_view methodToken, http::MethodBmp routeMethods) const noexcept;

  [[nodiscard]] bool requestHeadersAllowed(std::string_view headerList) const;

  void applyResponseHeaders(HttpResponse& response, std::string_view origin) const;

  [[nodiscard]] http::MethodBmp effectiveAllowedMethods(http::MethodBmp routeMethods) const noexcept {
    return _allowedMethods & routeMethods;
  }

  friend void detail::ApplyCorsPolicyData(detail::CorsPolicyData&& data, CorsPolicy& policy);
  friend detail::CorsPolicyData detail::BuildCorsPolicyData(const CorsPolicy& policy);

  ConcatenatedStrings32 _allowedOrigins;
  ConcatenatedHeaderValues _allowedRequestHeaders;
  ConcatenatedHeaderValues _exposedHeaders;
  std::chrono::seconds _maxAge{-1};
  http::MethodBmp _allowedMethods{http::Method::GET | http::Method::HEAD | http::Method::POST};
  OriginMode _originMode{OriginMode::Any};
  bool _allowCredentials{false};
  bool _allowPrivateNetwork{false};
  // Globally enable/disable this policy
  bool _active{false};
};

}  // namespace aeronet

#ifdef AERONET_ENABLE_GLAZE

#include <cassert>
#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>  // IWYU pragma: keep
#include <utility>

#include "aeronet/vector.hpp"

// ============================================================================
// CorsPolicy - single serialization-shape struct, shared by read and write.
// Only fields with no native JSON/YAML shape need a mirror:
//   - allowedMethods: bitmask <-> list of method name strings
//   - allowedOrigins/originMode: "*" wildcard <-> OriginMode::Any, plus CI dedup
//   - maxAgeSeconds: chrono::seconds <-> int64 with a -1 "unset" sentinel
// Everything else (ConcatenatedStrings32, ConcatenatedHeaderValues, bool) is
// natively (de)serializable, so it's used as-is - no mirrored vector<string>.
// ============================================================================
namespace aeronet::detail {

struct CorsPolicyData {
  bool active{false};
  bool allowCredentials{false};
  bool allowPrivateNetwork{false};
  ConcatenatedStrings32 allowedOrigins;
  vector<std::string> allowedMethods;
  ConcatenatedHeaderValues allowedRequestHeaders;
  ConcatenatedHeaderValues exposedHeaders;
  int64_t maxAgeSeconds{-1};
};

inline void ApplyCorsPolicyData(CorsPolicyData&& data, CorsPolicy& policy) {
  policy = CorsPolicy{};
  if (!data.active) {
    return;
  }

  policy._active = true;
  policy._allowCredentials = data.allowCredentials;
  policy._allowPrivateNetwork = data.allowPrivateNetwork;

  if (data.allowedOrigins.size() == 1 && data.allowedOrigins.fullString() == "*") {
    policy._originMode = CorsPolicy::OriginMode::Any;
  } else {
    policy._originMode = CorsPolicy::OriginMode::Enumerated;
    for (const auto& origin : data.allowedOrigins) {
      if (!origin.empty() && !policy._allowedOrigins.containsCI(origin)) {
        policy._allowedOrigins.append(origin);
      }
    }
  }

  http::MethodBmp methods{};
  for (const auto& methodStr : data.allowedMethods) {
    for (http::MethodIdx idx = 0; idx < http::kNbMethods; ++idx) {
      if (http::kMethodStrings[idx] == methodStr) {
        methods |= static_cast<http::MethodBmp>(1U << idx);
        break;
      }
    }
  }
  if (methods != 0) {
    policy._allowedMethods = methods;
  }

  policy._allowedRequestHeaders = std::move(data.allowedRequestHeaders);
  policy._exposedHeaders = std::move(data.exposedHeaders);

  if (data.maxAgeSeconds >= 0) {
    policy._maxAge = std::chrono::seconds{data.maxAgeSeconds};
  }
}

inline CorsPolicyData BuildCorsPolicyData(const CorsPolicy& policy) {
  CorsPolicyData data;
  data.active = policy._active;
  data.allowCredentials = policy._allowCredentials;
  data.allowPrivateNetwork = policy._allowPrivateNetwork;
  data.maxAgeSeconds = policy._maxAge.count();

  if (policy._originMode == CorsPolicy::OriginMode::Any) {
    data.allowedOrigins.append("*");
  } else {
    data.allowedOrigins = policy._allowedOrigins;
  }

  for (http::MethodIdx idx = 0; idx < http::kNbMethods; ++idx) {
    const auto method = http::MethodFromIdx(idx);
    if (http::IsMethodSet(policy._allowedMethods, method)) {
      data.allowedMethods.emplace_back(http::MethodToStr(method));
    }
  }

  data.allowedRequestHeaders = policy._allowedRequestHeaders;
  data.exposedHeaders = policy._exposedHeaders;

  return data;
}

}  // namespace aeronet::detail

template <>
struct glz::meta<aeronet::CorsPolicy> {
  static constexpr bool custom_read = true;
  static constexpr bool custom_write = true;
};

namespace glz {

template <uint32_t Format>
struct from<Format, aeronet::CorsPolicy> {
  template <auto Opts>
  static void op(aeronet::CorsPolicy& value, is_context auto&& ctx, auto&& it, auto&& end) {
    aeronet::detail::CorsPolicyData data;
    parse<Format>::template op<Opts>(data, ctx, it, end);
    assert(!static_cast<bool>(ctx.error));
    aeronet::detail::ApplyCorsPolicyData(std::move(data), value);
  }
};

template <uint32_t Format>
struct to<Format, aeronet::CorsPolicy> {
  template <auto Opts, is_context Ctx, class B, class IX>
  static void op(const aeronet::CorsPolicy& self, Ctx&& ctx, B&& b, IX&& ix) {
    const auto data = aeronet::detail::BuildCorsPolicyData(self);
    if constexpr (Format == YAML) {
      serialize<Format>::template op<yaml::flow_context_on<Opts>()>(data, ctx, b, ix);
    } else {
      serialize<Format>::template op<Opts>(data, ctx, b, ix);
    }
  }
};

}  // namespace glz

#endif