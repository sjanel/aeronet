#pragma once

#include <cassert>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>  // IWYU pragma: keep
#include <string>

#include "aeronet/cors-policy.hpp"
#include "aeronet/glaze-enum-adapters.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/vector.hpp"

// ============================================================================
// CorsPolicy - single serialization-shape struct, shared by read and write.
// The internal representation (bitmasks, ConcatenatedStrings32, chrono) differs
// from the flat JSON/YAML form, so a conversion aggregate is unavoidable.
// ============================================================================
namespace aeronet::detail {

struct CorsPolicyData {
  bool active{false};
  bool allowCredentials{false};
  bool allowPrivateNetwork{false};
  vector<std::string> allowedOrigins;
  vector<std::string> allowedMethods;
  vector<std::string> allowedRequestHeaders;
  vector<std::string> exposedHeaders;
  int64_t maxAgeSeconds{-1};
};

inline void ApplyCorsPolicyData(const CorsPolicyData& data, CorsPolicy& policy) {
  policy = CorsPolicy{};
  if (!data.active) {
    return;
  }

  policy._active = true;

  if (data.allowedOrigins.size() == 1 && data.allowedOrigins[0] == "*") {
    policy._originMode = CorsPolicy::OriginMode::Any;
  } else {
    policy._originMode = CorsPolicy::OriginMode::Enumerated;
    for (const auto& origin : data.allowedOrigins) {
      if (!origin.empty() && !policy._allowedOrigins.containsCI(origin)) {
        policy._allowedOrigins.append(origin);
      }
    }
  }

  http::MethodBmp methods{0};
  for (const auto& methodStr : data.allowedMethods) {
    for (int idx = 0; idx < http::kNbMethods; ++idx) {
      if (http::kMethodStrings[idx] == methodStr) {
        methods |= static_cast<http::MethodBmp>(1U << idx);
        break;
      }
    }
  }
  if (methods != 0) {
    policy._allowedMethods = methods;
  }

  if (data.allowedRequestHeaders.size() == 1 && data.allowedRequestHeaders[0] == "*") {
    policy._allowedRequestHeaders.append("*");
  } else {
    for (const auto& hdr : data.allowedRequestHeaders) {
      if (!hdr.empty()) {
        policy._allowedRequestHeaders.append(hdr);
      }
    }
  }

  for (const auto& hdr : data.exposedHeaders) {
    if (!hdr.empty()) {
      policy._exposedHeaders.append(hdr);
    }
  }

  policy._allowCredentials = data.allowCredentials;
  policy._allowPrivateNetwork = data.allowPrivateNetwork;

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
    data.allowedOrigins.emplace_back("*");
  } else {
    for (auto sv : policy._allowedOrigins) {
      data.allowedOrigins.emplace_back(sv);
    }
  }
  for (http::MethodIdx idx = 0; idx < http::kNbMethods; ++idx) {
    const auto method = http::MethodFromIdx(idx);
    if (http::IsMethodSet(policy._allowedMethods, method)) {
      data.allowedMethods.emplace_back(http::MethodToStr(method));
    }
  }
  for (auto sv : policy._allowedRequestHeaders) {
    data.allowedRequestHeaders.emplace_back(sv);
  }
  for (auto sv : policy._exposedHeaders) {
    data.exposedHeaders.emplace_back(sv);
  }

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
    // Glaze validated JSON/YAML structure before calling custom reader
    assert(!static_cast<bool>(ctx.error));
    aeronet::detail::ApplyCorsPolicyData(data, value);
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
