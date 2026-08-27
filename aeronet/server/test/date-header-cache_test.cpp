#include "aeronet/internal/date-header-cache.hpp"

#include <gtest/gtest.h>

#include <chrono>

#include "aeronet/timedef.hpp"

namespace aeronet::internal {
namespace {

using namespace std::chrono_literals;

TEST(DateHeaderCache, RefreshesAtNextWallClockSecondBoundary) {
  DateHeaderCache cache;
  const SteadyClock::time_point steadyBase{10s};
  SysTimePoint wallNow = SysTimePoint{} + 900ms;
  int wallClockReads = 0;
  auto readWallClock = [&] {
    ++wallClockReads;
    return wallNow;
  };

  cache.refreshIfNeeded(steadyBase, readWallClock);
  EXPECT_EQ(cache.view(), "Thu, 01 Jan 1970 00:00:00 GMT");
  EXPECT_EQ(wallClockReads, 1);

  wallNow = SysTimePoint{} + 999ms;
  cache.refreshIfNeeded(steadyBase + 99ms, readWallClock);
  EXPECT_EQ(cache.view(), "Thu, 01 Jan 1970 00:00:00 GMT");
  EXPECT_EQ(wallClockReads, 1);

  wallNow = SysTimePoint{} + 1s;
  cache.refreshIfNeeded(steadyBase + 100ms, readWallClock);
  EXPECT_EQ(cache.view(), "Thu, 01 Jan 1970 00:00:01 GMT");
  EXPECT_EQ(wallClockReads, 2);

  wallNow = SysTimePoint{} + 1999ms;
  cache.refreshIfNeeded(steadyBase + 1099ms, readWallClock);
  EXPECT_EQ(wallClockReads, 2);

  wallNow = SysTimePoint{} + 2s;
  cache.refreshIfNeeded(steadyBase + 1100ms, readWallClock);
  EXPECT_EQ(cache.view(), "Thu, 01 Jan 1970 00:00:02 GMT");
  EXPECT_EQ(wallClockReads, 3);
}

TEST(DateHeaderCache, UsesCurrentWallTimeAfterDelayedRefresh) {
  DateHeaderCache cache;
  const SteadyClock::time_point steadyBase{20s};
  SysTimePoint wallNow = SysTimePoint{} + 250ms;
  auto readWallClock = [&] { return wallNow; };

  cache.refreshIfNeeded(steadyBase, readWallClock);

  wallNow = SysTimePoint{} + 7s + 400ms;
  cache.refreshIfNeeded(steadyBase + 7s, readWallClock);
  EXPECT_EQ(cache.view(), "Thu, 01 Jan 1970 00:00:07 GMT");
}

TEST(DateHeaderCache, FollowsBackwardWallClockAdjustment) {
  DateHeaderCache cache;
  const SteadyClock::time_point steadyBase{30s};
  SysTimePoint wallNow = SysTimePoint{} + 10s + 900ms;
  auto readWallClock = [&] { return wallNow; };

  cache.refreshIfNeeded(steadyBase, readWallClock);
  EXPECT_EQ(cache.view(), "Thu, 01 Jan 1970 00:00:10 GMT");

  wallNow = SysTimePoint{} + 4s;
  cache.refreshIfNeeded(steadyBase + 100ms, readWallClock);
  EXPECT_EQ(cache.view(), "Thu, 01 Jan 1970 00:00:04 GMT");
}

}  // namespace
}  // namespace aeronet::internal
