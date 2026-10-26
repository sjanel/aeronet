#pragma once

#include <cassert>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>  // IWYU pragma: keep
#include <string>
#include <utility>

#include "aeronet/concatenated-header-values.hpp"
#include "aeronet/concatenated-strings.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/http-method.hpp"
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
    methods |= static_cast<http::MethodBmp>(http::ParseMethodStr(methodStr));
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
  static void op(const aeronet::CorsPolicy& self, Ctx&& ctx, B&& buffer, IX&& ix) {
    const auto data = aeronet::detail::BuildCorsPolicyData(self);
    if constexpr (Format == YAML) {
      serialize<Format>::template op<yaml::flow_context_on<Opts>()>(data, ctx, buffer, ix);
    } else {
      serialize<Format>::template op<Opts>(data, ctx, buffer, ix);
    }
  }
};

}  // namespace glz
