// Pure unit coverage for RetryConfig: the backoff math (delayFor), idempotency classification and the
// retryable-status predicate. No sockets involved.
#include "aeronet/retry-config.hpp"

#include <gtest/gtest.h>

#include <chrono>

#include "aeronet/http-status-code.hpp"

namespace aeronet {

namespace {

bool ShouldRetryStatus(const RetryConfig& cfg, http::StatusCode code) {
  return std::ranges::find(cfg.retryStatuses, code) != cfg.retryStatuses.end();
}

}  // namespace

using namespace std::chrono_literals;

TEST(RetryConfigTest, DefaultsAreConservative) {
  RetryConfig retry;
  EXPECT_EQ(retry.maxAttempts, 1U);  // backoff retries off by default
  EXPECT_EQ(retry.baseDelay, 100ms);
  EXPECT_EQ(retry.maxDelay, 2s);
  EXPECT_FLOAT_EQ(retry.multiplier, 2.0F);
  EXPECT_FLOAT_EQ(retry.jitter, 0.0F);
  EXPECT_FALSE(retry.retryIdempotentAfterSend);
  EXPECT_TRUE(retry.honorRetryAfter);
  // 429 and 503 are retried out of the box; nothing else.
  EXPECT_TRUE(ShouldRetryStatus(retry, http::StatusCodeTooManyRequests));
  EXPECT_TRUE(ShouldRetryStatus(retry, http::StatusCodeServiceUnavailable));
  EXPECT_FALSE(ShouldRetryStatus(retry, http::StatusCodeInternalServerError));
  EXPECT_FALSE(ShouldRetryStatus(retry, http::StatusCodeOK));
}

TEST(RetryConfigTest, DelayGrowsExponentiallyAndCaps) {
  RetryConfig retry;
  retry.baseDelay = 100ms;
  retry.maxDelay = 1s;
  retry.multiplier = 2.0F;
  EXPECT_EQ(retry.delayFor(0), 100ms);  // first retry: base
  EXPECT_EQ(retry.delayFor(1), 200ms);
  EXPECT_EQ(retry.delayFor(2), 400ms);
  EXPECT_EQ(retry.delayFor(3), 800ms);
  EXPECT_EQ(retry.delayFor(4), 1s);  // would be 1600ms -> capped at maxDelay
  EXPECT_EQ(retry.delayFor(50), 1s);
}

TEST(RetryConfigTest, JitterStaysWithinBoundsAndIsDeterministicAtMidpoint) {
  RetryConfig retry;
  retry.baseDelay = 100ms;
  retry.maxDelay = 10s;
  retry.multiplier = 1.0F;  // keep the deterministic part flat at 100ms
  retry.jitter = 0.5F;      // factor in [0.5, 1.5]
  // rnd01 == 0.5 -> factor 1.0 -> unchanged.
  EXPECT_EQ(retry.delayFor(0, 0.5), 100ms);
  // Extremes of the random unit hit the bounds.
  EXPECT_EQ(retry.delayFor(0, 0.0), 50ms);                                       // factor 0.5
  EXPECT_NEAR(static_cast<double>(retry.delayFor(0, 1.0).count()), 150.0, 1.0);  // factor 1.5
  // With jitter == 0 the random argument is ignored entirely.
  retry.jitter = 0.0F;
  EXPECT_EQ(retry.delayFor(0, 0.0), 100ms);
  EXPECT_EQ(retry.delayFor(0, 1.0), 100ms);
}

TEST(RetryConfigTest, JitterIsClampedToMaxDelay) {
  RetryConfig retry;
  retry.baseDelay = 100ms;
  retry.maxDelay = 120ms;
  retry.multiplier = 1.0F;
  retry.jitter = 1.0F;  // factor up to 2.0 -> 200ms, clamped to 120ms
  EXPECT_EQ(retry.delayFor(0, 1.0), 120ms);
}

TEST(RetryConfigTest, RetryStatusesAreConfigurable) {
  RetryConfig retry;
  retry.retryStatuses = {http::StatusCodeBadGateway, http::StatusCodeGatewayTimeout};
  EXPECT_TRUE(ShouldRetryStatus(retry, http::StatusCodeBadGateway));
  EXPECT_TRUE(ShouldRetryStatus(retry, http::StatusCodeGatewayTimeout));
  EXPECT_FALSE(ShouldRetryStatus(retry, http::StatusCodeServiceUnavailable));  // no longer in the set
  retry.retryStatuses.clear();
  EXPECT_FALSE(ShouldRetryStatus(retry, http::StatusCodeBadGateway));  // empty -> nothing retried
}

}  // namespace aeronet
