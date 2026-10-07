#include "aeronet/buffer-cache.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <utility>

#include "aeronet/asan-poison.hpp"

namespace aeronet::internal {

TEST(BufferCacheTest, ReusesDeallocatedBufferIfLargeEnough) {
  BufferCache cache;

  void* buf1 = cache.allocate(64);
  ASSERT_NE(buf1, nullptr);
  std::memset(buf1, 'a', 64);
  cache.deallocate(buf1);

  void* buf2 = cache.allocate(32);
  EXPECT_EQ(buf2, buf1);
  std::memset(buf2, 'b', 32);
  cache.deallocate(buf2);

  void* buf3 = cache.allocate(128);
  ASSERT_NE(buf3, nullptr);
  std::memset(buf3, 'c', 128);
  cache.deallocate(buf3);
}

TEST(BufferCacheTest, FreesUntrackedBuffers) {
  BufferCache cache;

  void* buf1 = cache.allocate(16);
  void* buf2 = cache.allocate(16);  // nothing cached, buf2 is now the tracked buffer
  ASSERT_NE(buf1, nullptr);
  ASSERT_NE(buf2, nullptr);
  EXPECT_NE(buf1, buf2);

  cache.deallocate(buf1);  // untracked, freed
  cache.deallocate(buf2);  // cached
  EXPECT_EQ(cache.allocate(16), buf2);
  cache.deallocate(buf2);
}

TEST(BufferCacheTest, MoveTransfersCachedBuffer) {
  BufferCache cache;
  void* buf = cache.allocate(16);
  cache.deallocate(buf);

  BufferCache moved(std::move(cache));
  EXPECT_EQ(moved.allocate(16), buf);
  moved.deallocate(buf);

  BufferCache other;
  other.deallocate(other.allocate(8));
  other = std::move(moved);
  EXPECT_EQ(other.allocate(16), buf);
  other.deallocate(buf);
}

TEST(BufferCacheTest, AsanPoisonsCachedBuffer) {
  if (!AERONET_ASAN_ENABLED) {
    GTEST_SKIP() << "AddressSanitizer is not enabled";
  }
  BufferCache cache;

  auto* buf = static_cast<char*>(cache.allocate(64));
  EXPECT_FALSE(AsanIsPoisoned(buf + 63));

  cache.deallocate(buf);
  EXPECT_TRUE(AsanIsPoisoned(buf));
  EXPECT_TRUE(AsanIsPoisoned(buf + 63));

  // Only the requested size of the larger cached buffer is unpoisoned
  ASSERT_EQ(cache.allocate(20), buf);
  EXPECT_FALSE(AsanIsPoisoned(buf + 19));
  EXPECT_TRUE(AsanIsPoisoned(buf + 20));

  cache.deallocate(buf);
  EXPECT_TRUE(AsanIsPoisoned(buf));
}

TEST(BufferCacheDeathTest, AsanReportsUseAfterDeallocate) {
  if (!AERONET_ASAN_ENABLED) {
    GTEST_SKIP() << "AddressSanitizer is not enabled";
  }
  BufferCache cache;
  auto* buf = static_cast<char*>(cache.allocate(64));
  cache.deallocate(buf);

  EXPECT_DEATH({ [[maybe_unused]] volatile char ch = buf[0]; }, "use-after-poison");
}

}  // namespace aeronet::internal
