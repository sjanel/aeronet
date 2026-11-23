#include "../src/span-sampler.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace aeronet::tracing::detail {

namespace {

int CountSampled(SpanSampler& sampler, int nbDraws) {
  int nbSampled = 0;
  for (int drawPos = 0; drawPos < nbDraws; ++drawPos) {
    nbSampled += sampler.sample() ? 1 : 0;
  }
  return nbSampled;
}

}  // namespace

TEST(SpanSampler, OneSamplesAllZeroNone) {
  SpanSampler all(1.0F, 42);
  EXPECT_EQ(CountSampled(all, 10000), 10000);
  SpanSampler none(0.0F, 42);
  EXPECT_EQ(CountSampled(none, 10000), 0);
  SpanSampler nanRate(std::numeric_limits<float>::quiet_NaN(), 42);
  EXPECT_EQ(CountSampled(nanRate, 10000), 0);
}

TEST(SpanSampler, RateIsTheProportionOfSampledSpans) {
  for (const float rate : {0.001F, 0.25F, 0.5F, 0.9F, 1.0F - 1e-7F}) {
    for (const uint64_t seed : {uint64_t{0}, uint64_t{7}, std::numeric_limits<uint64_t>::max()}) {
      SpanSampler sampler(rate, seed);
      static constexpr int kNbDraws = 100000;
      const double proportion = static_cast<double>(CountSampled(sampler, kNbDraws)) / kNbDraws;
      // More than 6 standard deviations for these draws (deterministic for a seed).
      EXPECT_NEAR(proportion, static_cast<double>(rate), 0.01) << rate << ' ' << seed;
    }
  }
}

}  // namespace aeronet::tracing::detail
