#include "aeronet/internal/date-header-cache.hpp"

#include <gtest/gtest.h>

#include <ctime>
#include <string_view>

#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"

namespace aeronet::internal {
namespace {

// The wall clock of the tests, in seconds since the epoch, and the number of times the cache read it.
std::time_t gWallSeconds = 0;
int gWallClockReads = 0;

std::time_t FakeWallSeconds(std::time_t* /*unused*/) {
  ++gWallClockReads;
  return gWallSeconds;
}

std::string_view Current(DateHeaderCache& cache) { return {cache.current(), RFC7231DateStrLen}; }

class DateHeaderCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    gWallSeconds = 0;
    gWallClockReads = 0;
  }

  DateHeaderCache cache{&FakeWallSeconds};
};

TEST_F(DateHeaderCacheTest, ReadsTheWallClockAtEachCall) {
  EXPECT_EQ(Current(cache), "Thu, 01 Jan 1970 00:00:00 GMT");
  EXPECT_EQ(Current(cache), "Thu, 01 Jan 1970 00:00:00 GMT");
  EXPECT_EQ(gWallClockReads, 2);

  // No event loop iteration in between: a slow handler, or the requests served after it.
  gWallSeconds = 1;
  EXPECT_EQ(Current(cache), "Thu, 01 Jan 1970 00:00:01 GMT");
  EXPECT_EQ(gWallClockReads, 3);
}

TEST_F(DateHeaderCacheTest, KeepsTheBufferOfTheSecond) {
  const char* first = cache.current();
  EXPECT_EQ(cache.current(), first);
  gWallSeconds = 2;
  EXPECT_EQ(cache.current(), first);  // refreshed in place
  EXPECT_EQ(Current(cache), "Thu, 01 Jan 1970 00:00:02 GMT");
}

TEST_F(DateHeaderCacheTest, FollowsALargeForwardJump) {
  EXPECT_EQ(Current(cache), "Thu, 01 Jan 1970 00:00:00 GMT");
  gWallSeconds = 784111777;
  EXPECT_EQ(Current(cache), "Sun, 06 Nov 1994 08:49:37 GMT");
}

TEST_F(DateHeaderCacheTest, FollowsBackwardWallClockAdjustment) {
  gWallSeconds = 10;
  EXPECT_EQ(Current(cache), "Thu, 01 Jan 1970 00:00:10 GMT");

  gWallSeconds = 4;
  EXPECT_EQ(Current(cache), "Thu, 01 Jan 1970 00:00:04 GMT");
}

TEST_F(DateHeaderCacheTest, FormatsTheFirstReadEvenAtTheSmallestTime) {
  // The initial cached second is the smallest time_t: the first value read, whatever it is, is formatted.
  gWallSeconds = -1;
  EXPECT_EQ(Current(cache), "Wed, 31 Dec 1969 23:59:59 GMT");
}

TEST(DateHeaderCache, DefaultReadsTheSystemWallClock) {
  DateHeaderCache cache;
  const SysTimePoint before = SysClock::from_time_t(std::time(nullptr));
  const SysTimePoint date = TryParseTimeRFC7231(Current(cache));
  const SysTimePoint after = SysClock::from_time_t(std::time(nullptr));
  ASSERT_NE(date, kInvalidTimePoint);
  EXPECT_GE(date, before);
  EXPECT_LE(date, after);
}

}  // namespace
}  // namespace aeronet::internal
