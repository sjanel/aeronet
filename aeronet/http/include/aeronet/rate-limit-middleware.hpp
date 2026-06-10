#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "aeronet/middleware.hpp"
#include "aeronet/rate-limit.hpp"

namespace aeronet {

enum class RateLimitClientKeyStrategy : uint8_t { PeerAddress, XForwardedForFirst, HeaderValue, Custom };

struct RateLimitRequestMiddlewareBuilder {
  // Build a request middleware that enforces rate limits using the configured key strategy.
  // Returns 429 Too Many Requests with Retry-After when over the limit.
  [[nodiscard]] RequestMiddleware build() const&;
  [[nodiscard]] RequestMiddleware build() &&;

  RateLimitConfig config;
  RateLimitStorePtr store;
  RateLimitClientKeyStrategy keyStrategy{RateLimitClientKeyStrategy::PeerAddress};
  std::string_view headerName{"x-forwarded-for"};
  std::function<std::string_view(const HttpRequest&)> customKeyExtractor;
  std::string_view rejectionBody{"rate limited"};
};

}  // namespace aeronet
