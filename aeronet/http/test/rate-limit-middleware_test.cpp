#include "aeronet/rate-limit-middleware.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

#include "aeronet/lower-ascii-key.hpp"

namespace aeronet {

TEST(RateLimitMiddleware, HeaderValueStrategyRequiresHeaderName) {
  RateLimitRequestMiddlewareBuilder options;
  options.keyStrategy = RateLimitClientKeyStrategy::HeaderValue;
  options.headerName = {};

  EXPECT_THROW(static_cast<void>(options.build()), std::invalid_argument);
}

}  // namespace aeronet