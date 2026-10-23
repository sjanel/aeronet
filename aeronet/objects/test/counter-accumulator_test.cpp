#include "../src/counter-accumulator.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/metric-label.hpp"
#include "aeronet/sys-test-support.hpp"

namespace aeronet::tracing::detail {

namespace {

struct FlushedCounter {
  bool operator==(const FlushedCounter&) const = default;
  auto operator<=>(const FlushedCounter&) const = default;

  std::string name;
  uint64_t delta{};
  // "key=value" of each label, in order
  std::vector<std::string> labels;
};

// Flushed counters, sorted: their flush order is the one of the hash map.
std::vector<FlushedCounter> Flush(CounterAccumulator& accumulator) {
  std::vector<FlushedCounter> counters;
  accumulator.flush([&counters](std::string_view name, uint64_t delta, MetricLabels labels) {
    FlushedCounter& counter = counters.emplace_back(std::string(name), delta);
    for (const auto& [key, value] : labels) {
      counter.labels.push_back(std::string(key) + '=' + std::string(value));
    }
  });
  std::ranges::sort(counters);
  return counters;
}

}  // namespace

TEST(CounterAccumulator, FlushWithoutCounters) {
  CounterAccumulator accumulator;
  EXPECT_TRUE(Flush(accumulator).empty());
}

TEST(CounterAccumulator, SumsPerNameAndLabels) {
  CounterAccumulator accumulator;
  const MetricLabel h2[]{{"protocol", "h2"}};
  const MetricLabel h1[]{{"protocol", "http/1.1"}};
  const MetricLabel twoLabels[]{{"direction", "sent"}, {"frame.type", "headers"}};

  accumulator.add("requests", 1, {});
  accumulator.add("requests", 2, {});
  accumulator.add("requests", 5, h2);
  accumulator.add("requests", 7, h2);
  accumulator.add("requests", 4, h1);
  accumulator.add("frames", 3, twoLabels);
  accumulator.add("frames", 1, twoLabels);

  const std::vector<FlushedCounter> expected{
      {"frames", 4, {"direction=sent", "frame.type=headers"}},
      {"requests", 3, {}},
      {"requests", 4, {"protocol=http/1.1"}},
      {"requests", 12, {"protocol=h2"}},
  };
  EXPECT_EQ(Flush(accumulator), expected);
}

TEST(CounterAccumulator, FlushResetsCounters) {
  CounterAccumulator accumulator;
  const MetricLabel labels[]{{"protocol", "h2"}};

  accumulator.add("requests", 2, labels);
  accumulator.add("bytes", 100, {});
  EXPECT_EQ(Flush(accumulator).size(), 2U);
  EXPECT_TRUE(Flush(accumulator).empty());

  accumulator.add("requests", 3, labels);
  const std::vector<FlushedCounter> expected{{"requests", 3, {"protocol=h2"}}};
  EXPECT_EQ(Flush(accumulator), expected);
}

// The OpenTelemetry SDK reads instrument names as C strings.
TEST(CounterAccumulator, FlushedNamesAreNullTerminated) {
  CounterAccumulator accumulator;
  const MetricLabel labels[]{{"protocol", "h2"}};
  accumulator.add("requests", 1, {});
  accumulator.add("frames", 1, labels);

  int nbFlushed = 0;
  accumulator.flush([&nbFlushed](std::string_view name, uint64_t, MetricLabels) {
    // Reads the byte after the view: name[name.size()] would be out of its range.
    EXPECT_EQ(name.data()[name.size()], '\0');  // NOLINT(readability-simplify-subscript-expr)
    ++nbFlushed;
  });
  EXPECT_EQ(nbFlushed, 2);
}

TEST(CounterAccumulator, ZeroDeltaIsNotFlushed) {
  CounterAccumulator accumulator;
  accumulator.add("requests", 0, {});
  EXPECT_TRUE(Flush(accumulator).empty());
}

TEST(CounterAccumulator, EmptyLabelKeyAndValue) {
  CounterAccumulator accumulator;
  const MetricLabel emptyLabel[]{{"", ""}};
  const MetricLabel emptyValues[]{{"a", ""}, {"", "b"}};

  accumulator.add("", 1, {});
  accumulator.add("requests", 1, {});
  accumulator.add("requests", 2, emptyLabel);
  accumulator.add("requests", 3, emptyValues);

  const std::vector<FlushedCounter> expected{
      {"", 1, {}},
      {"requests", 1, {}},
      {"requests", 2, {"="}},
      {"requests", 3, {"a=", "=b"}},
  };
  EXPECT_EQ(Flush(accumulator), expected);
}

TEST(CounterAccumulator, MoreLabelsThanPreviousCounters) {
  CounterAccumulator accumulator;
  const MetricLabel oneLabel[]{{"k1", "v1"}};
  const MetricLabel fiveLabels[]{{"k1", "v1"}, {"k2", "v2"}, {"k3", "v3"}, {"k4", "v4"}, {"k5", "v5"}};

  accumulator.add("one", 1, oneLabel);
  accumulator.add("five", 5, fiveLabels);

  const std::vector<FlushedCounter> expected{
      {"five", 5, {"k1=v1", "k2=v2", "k3=v3", "k4=v4", "k5=v5"}},
      {"one", 1, {"k1=v1"}},
  };
  EXPECT_EQ(Flush(accumulator), expected);
}

TEST(CounterAccumulator, ManyCountersAndLongLabels) {
  CounterAccumulator accumulator;
  static constexpr std::size_t kNbCounters = 2000;
  const std::string longValue(4096, 'v');
  std::vector<std::string> names;
  names.reserve(kNbCounters);
  for (std::size_t counterIdx = 0; counterIdx < kNbCounters; ++counterIdx) {
    names.push_back("counter." + std::to_string(counterIdx));
  }
  const MetricLabel labels[]{{"long", longValue}};
  for (int round = 0; round < 3; ++round) {
    for (std::size_t counterIdx = 0; counterIdx < kNbCounters; ++counterIdx) {
      accumulator.add(names[counterIdx], counterIdx, labels);
    }
  }

  const std::vector<FlushedCounter> flushed = Flush(accumulator);
  ASSERT_EQ(flushed.size(), kNbCounters - 1U);  // counter.0 only added zeros
  for (const FlushedCounter& counter : flushed) {
    const auto counterIdx = std::stoul(counter.name.substr(std::string_view("counter.").size()));
    EXPECT_EQ(counter.delta, 3U * counterIdx);
    ASSERT_EQ(counter.labels.size(), 1U);
    EXPECT_EQ(counter.labels[0], "long=" + longValue);
  }
}

#if AERONET_WANT_MALLOC_OVERRIDES
TEST(CounterAccumulator, AllocationFailureDropsTheIncrement) {
  CounterAccumulator accumulator;
  const MetricLabel labels[]{{"protocol", "h2"}};

  test::FailNextMalloc(1);
  accumulator.add("requests", 1, labels);
  test::ResetToSysMalloc();
  EXPECT_TRUE(Flush(accumulator).empty());

  accumulator.add("requests", 2, labels);
  const std::vector<FlushedCounter> expected{{"requests", 2, {"protocol=h2"}}};
  EXPECT_EQ(Flush(accumulator), expected);
}
#endif

}  // namespace aeronet::tracing::detail
