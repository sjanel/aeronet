#include "aeronet/adaptive-poll-timeout.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace aeronet;

// ---- baseTimeout checks ----

TEST(PollTimeoutPolicyValidateTest, BaseTimeoutZeroThrows) {
  EXPECT_THROW(
      (PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{0}, .minFactor = 0.0F, .maxFactor = 1.0F}.validate()),
      std::invalid_argument);
}

TEST(PollTimeoutPolicyValidateTest, BaseTimeoutNegativeThrows) {
  EXPECT_THROW((PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{-1}, .minFactor = 0.0F, .maxFactor = 1.0F}
                    .validate()),
               std::invalid_argument);
}

// ---- minTimeout checks ----

TEST(PollTimeoutPolicyValidateTest, MinTimeoutNegativeThrows) {
  EXPECT_THROW((PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{10}, .minFactor = -0.1F, .maxFactor = 1.0F}
                    .validate()),
               std::invalid_argument);
}

TEST(PollTimeoutPolicyValidateTest, MinTimeoutAboveOneThrows) {
  EXPECT_THROW((PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{10}, .minFactor = 1.5F, .maxFactor = 2.0F}
                    .validate()),
               std::invalid_argument);
}

TEST(PollTimeoutPolicyValidateTest, MinTimeoutNanThrows) {
  EXPECT_THROW((PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{10},
                                  .minFactor = std::numeric_limits<float>::quiet_NaN(),
                                  .maxFactor = 1.0F}
                    .validate()),
               std::invalid_argument);
}

TEST(PollTimeoutPolicyValidateTest, MinTimeoutInfinityThrows) {
  EXPECT_THROW((PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{10},
                                  .minFactor = std::numeric_limits<float>::infinity(),
                                  .maxFactor = 1.0F}
                    .validate()),
               std::invalid_argument);
}

// ---- maxTimeout checks ----

TEST(PollTimeoutPolicyValidateTest, MaxTimeoutBelowOneThrows) {
  EXPECT_THROW((PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{10}, .minFactor = 0.0F, .maxFactor = 0.5F}
                    .validate()),
               std::invalid_argument);
}

TEST(PollTimeoutPolicyValidateTest, MaxTimeoutNanThrows) {
  EXPECT_THROW((PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{10},
                                  .minFactor = 0.0F,
                                  .maxFactor = std::numeric_limits<float>::quiet_NaN()}
                    .validate()),
               std::invalid_argument);
}

TEST(PollTimeoutPolicyValidateTest, MaxTimeoutInfinityThrows) {
  EXPECT_THROW((PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{10},
                                  .minFactor = 0.0F,
                                  .maxFactor = std::numeric_limits<float>::infinity()}
                    .validate()),
               std::invalid_argument);
}

// ---- overflow check ----

TEST(PollTimeoutPolicyValidateTest, MaxTimeoutOverflowsIntThrows) {
  // base * maxFactor exceeds INT_MAX ms
  EXPECT_THROW((PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{std::numeric_limits<int>::max()},
                                  .minFactor = 0.0F,
                                  .maxFactor = 2.0F}
                    .validate()),
               std::invalid_argument);
}

// ---- valid policies ----

TEST(PollTimeoutPolicyValidateTest, ValidFixedPolicyDoesNotThrow) {
  EXPECT_NO_THROW((PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{10}, .minFactor = 0.0F, .maxFactor = 1.0F}
                       .validate()));
}

TEST(PollTimeoutPolicyValidateTest, ValidAdaptivePolicyDoesNotThrow) {
  EXPECT_NO_THROW((PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{10}, .minFactor = 0.5F, .maxFactor = 4.0F}
                       .validate()));
}

TEST(PollTimeoutPolicyValidateTest, ValidMaxAtIntMaxBoundaryDoesNotThrow) {
  // base * 1.0 == INT_MAX ms exactly → just within limit
  EXPECT_NO_THROW((PollTimeoutPolicy{
      .baseTimeout = std::chrono::milliseconds{std::numeric_limits<int>::max()}, .minFactor = 0.0F, .maxFactor = 1.0F}
                       .validate()));
}
