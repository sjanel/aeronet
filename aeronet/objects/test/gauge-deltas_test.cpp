#include "../src/gauge-deltas.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "aeronet/metric-label.hpp"
#include "aeronet/sys-test-support.hpp"

namespace aeronet::tracing::detail {

TEST(GaugeDeltas, FirstValueThenDifferences) {
  GaugeDeltas deltas;
  EXPECT_EQ(deltas.update("connections", {}, 5), 5);
  EXPECT_EQ(deltas.update("connections", {}, 5), 0);
  EXPECT_EQ(deltas.update("connections", {}, 12), 7);
  EXPECT_EQ(deltas.update("connections", {}, 3), -9);
  EXPECT_EQ(deltas.update("connections", {}, -4), -7);
  EXPECT_EQ(deltas.update("connections", {}, 0), 4);
}

TEST(GaugeDeltas, FirstValueZero) {
  GaugeDeltas deltas;
  EXPECT_EQ(deltas.update("connections", {}, 0), 0);
  EXPECT_EQ(deltas.update("connections", {}, 2), 2);
}

TEST(GaugeDeltas, PerNameAndLabels) {
  GaugeDeltas deltas;
  const MetricLabel h2[]{{"protocol", "h2"}};
  const MetricLabel h1[]{{"protocol", "http/1.1"}};
  const MetricLabel twoLabels[]{{"protocol", "h2"}, {"tls", "on"}};

  EXPECT_EQ(deltas.update("connections", {}, 10), 10);
  EXPECT_EQ(deltas.update("connections", h2, 4), 4);
  EXPECT_EQ(deltas.update("connections", h1, 6), 6);
  EXPECT_EQ(deltas.update("connections", twoLabels, 1), 1);
  EXPECT_EQ(deltas.update("streams", h2, 8), 8);

  EXPECT_EQ(deltas.update("connections", {}, 11), 1);
  EXPECT_EQ(deltas.update("connections", h2, 2), -2);
  EXPECT_EQ(deltas.update("connections", h1, 6), 0);
  EXPECT_EQ(deltas.update("connections", twoLabels, 3), 2);
  EXPECT_EQ(deltas.update("streams", h2, 5), -3);
}

// The pieces of a key are delimited: moving characters between the name, the label keys and the label values gives
// another gauge.
TEST(GaugeDeltas, PiecesDoNotMerge) {
  GaugeDeltas deltas;
  const MetricLabel labelB[]{{"b", ""}};
  const MetricLabel valueB[]{{"", "b"}};
  const MetricLabel labelBc[]{{"bc", ""}};
  const MetricLabel labelBValueC[]{{"b", "c"}};

  EXPECT_EQ(deltas.update("ab", {}, 1), 1);
  EXPECT_EQ(deltas.update("a", labelB, 2), 2);
  EXPECT_EQ(deltas.update("a", valueB, 3), 3);
  EXPECT_EQ(deltas.update("a", labelBc, 4), 4);
  EXPECT_EQ(deltas.update("a", labelBValueC, 5), 5);
  EXPECT_EQ(deltas.update("", {}, 6), 6);
}

TEST(GaugeDeltas, ExtremeValues) {
  GaugeDeltas deltas;
  static constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  static constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  EXPECT_EQ(deltas.update("big", {}, kMax), kMax);
  EXPECT_EQ(deltas.update("big", {}, 0), -kMax);
  EXPECT_EQ(deltas.update("small", {}, kMin), kMin);
  EXPECT_EQ(deltas.update("small", {}, -1), kMax);
}

TEST(GaugeDeltas, ManyGaugesAndLongLabels) {
  GaugeDeltas deltas;
  static constexpr std::size_t kNbGauges = 2000;
  const std::string longValue(4096, 'v');
  std::vector<std::string> names;
  names.reserve(kNbGauges);
  for (std::size_t gaugeIdx = 0; gaugeIdx < kNbGauges; ++gaugeIdx) {
    names.push_back("gauge." + std::to_string(gaugeIdx));
  }
  const MetricLabel labels[]{{"long", longValue}};
  for (int64_t round = 1; round <= 3; ++round) {
    for (std::size_t gaugeIdx = 0; gaugeIdx < kNbGauges; ++gaugeIdx) {
      const auto value = static_cast<int64_t>(gaugeIdx) * round;
      // The first round adds the whole value, each next one adds gaugeIdx again.
      EXPECT_EQ(deltas.update(names[gaugeIdx], labels, value), static_cast<int64_t>(gaugeIdx));
    }
  }
}

#if AERONET_WANT_MALLOC_OVERRIDES
TEST(GaugeDeltas, AllocationFailureRemembersNothing) {
  GaugeDeltas deltas;
  const MetricLabel labels[]{{"protocol", "h2"}};

  test::FailNextMalloc(1);
  EXPECT_THROW(deltas.update("connections", labels, 4), std::bad_alloc);
  test::ResetToSysMalloc();

  EXPECT_EQ(deltas.update("connections", labels, 7), 7);
}
#endif

}  // namespace aeronet::tracing::detail
