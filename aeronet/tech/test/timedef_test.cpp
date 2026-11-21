#include "aeronet/timedef.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace aeronet {

TEST(DeadlineAfter, AddsTheTimeout) {
  const SteadyClock::time_point start = SteadyClock::now();
  EXPECT_EQ(DeadlineAfter(start, std::chrono::milliseconds{5}), start + std::chrono::milliseconds{5});
  EXPECT_EQ(DeadlineAfter(start, std::chrono::hours{24 * 365}), start + std::chrono::hours{24 * 365});
}

TEST(DeadlineAfter, SaturatesBeyondTheRangeOfTheClock) {
  const SteadyClock::time_point start = SteadyClock::now();
  EXPECT_EQ(DeadlineAfter(start, std::chrono::milliseconds::max()), SteadyClock::time_point::max());
  EXPECT_EQ(DeadlineAfter(start, std::chrono::milliseconds::max() / 2), SteadyClock::time_point::max());
  // The largest timeout the clock can add to the epoch: the next millisecond would overflow its nanoseconds.
  const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::duration::max());
  EXPECT_EQ(DeadlineAfter(SteadyClock::time_point{}, room - std::chrono::milliseconds{1}),
            SteadyClock::time_point{} + (room - std::chrono::milliseconds{1}));
  EXPECT_EQ(DeadlineAfter(SteadyClock::time_point{}, room), SteadyClock::time_point::max());
}

static_assert(DeadlineAfter(SteadyClock::time_point{}, std::chrono::milliseconds::max()) ==
              SteadyClock::time_point::max());

}  // namespace aeronet
