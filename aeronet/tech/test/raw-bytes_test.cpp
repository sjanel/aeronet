#include "raw-bytes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "vector.hpp"

namespace aeronet {

using BytesView = std::span<const std::byte>;

namespace {
// Helper: create a vector<std::byte> from a string for iterator tests
vector<std::byte> toCharVec(std::string_view str) {
  return {reinterpret_cast<const std::byte *>(str.data()),
          reinterpret_cast<const std::byte *>(str.data()) + str.size()};
}

// Helper: produce a BytesView (span<const std::byte>) from any string-like view
BytesView toBytes(std::string_view sv) { return {reinterpret_cast<const std::byte *>(sv.data()), sv.size()}; }

}  // namespace

TEST(SimpleBufferTest, DefaultConstructor) {
  RawBytes buf;  // Explicit <> to avoid CTAD selecting the span constructor
  EXPECT_EQ(buf.size(), 0);
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.data(), nullptr);
  EXPECT_EQ(buf.begin(), buf.end());
  EXPECT_EQ(buf.data(), buf.data() + buf.size());
}

TEST(SimpleBufferTest, RandomAccessIteratorConstructor) {
  std::string text = "Hello";
  RawBytes buf(reinterpret_cast<const std::byte *>(text.data()),
               reinterpret_cast<const std::byte *>(text.data()) + text.size());
  EXPECT_EQ(buf.size(), text.size());
  BytesView bufView(buf);
  auto textView = toBytes(text);
  EXPECT_TRUE(std::equal(bufView.data(), bufView.data() + bufView.size(), textView.data()));
}

TEST(SimpleBufferTest, MoveConstructor) {
  auto data = toCharVec("move-me");
  RawBytes buf1(data.begin(), data.end());
  auto *oldPtr = buf1.data();

  RawBytes buf2(std::move(buf1));
  EXPECT_EQ(buf2.data(), oldPtr);

  BytesView buf2View(buf2);
  BytesView dataView(data.data(), data.size());

  EXPECT_EQ(buf2.size(), data.size());
  EXPECT_TRUE(std::equal(buf2View.data(), buf2View.data() + buf2View.size(), dataView.data()));
}

TEST(SimpleBufferTest, MoveAssignment) {
  auto data = toCharVec("move-me");
  RawBytes buf1(data.begin(), data.end());
  auto *oldPtr = buf1.data();

  RawBytes buf2;
  buf2 = std::move(buf1);
  EXPECT_EQ(buf2.data(), oldPtr);

  BytesView buf2View(buf2);
  BytesView dataView(data.data(), data.size());

  EXPECT_EQ(buf2.size(), data.size());
  EXPECT_TRUE(std::equal(buf2View.data(), buf2View.data() + buf2View.size(), dataView.data()));
}

TEST(SimpleBufferTest, RangeForLoop) {
  std::string text = "range";
  RawBytes buf(reinterpret_cast<const std::byte *>(text.data()),
               reinterpret_cast<const std::byte *>(text.data()) + text.size());
  std::string collected;
  for (auto ch : buf) {
    collected.push_back(static_cast<char>(ch));
  }
  EXPECT_EQ(collected, text);
}

TEST(SimpleBufferTest, RangesAlgorithmsWork) {
  vector<std::byte> text({std::byte('x'), std::byte('y'), std::byte('z')});
  RawBytes buf(text.begin(), text.end());

  // ranges::equal
  EXPECT_TRUE(std::ranges::equal(buf, text));
  // ranges::copy
  ASSERT_LE(buf.size(), static_cast<size_t>(std::numeric_limits<vector<std::byte>::size_type>::max()));
  vector<std::byte> copied(static_cast<vector<std::byte>::size_type>(buf.size()));
  std::ranges::copy(buf, copied.begin());
  std::ranges::copy(buf, copied.begin());
  EXPECT_EQ(copied, text);
}

TEST(RawBytesResizeAndOverwrite, BasicAppendAndShrink) {
  RawBytes buf;  // empty
  // Append 10 bytes
  buf.resize_and_overwrite(10, [](std::byte *base, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      base[i] = static_cast<std::byte>(i);
    }
    return n;  // keep all 10
  });
  ASSERT_EQ(buf.size(), 10U);
  for (std::size_t i = 0; i < buf.size(); ++i) {
    ASSERT_EQ(static_cast<unsigned>(buf[i]), i);
  }

  // Grow by 6 more bytes, but pretend only 4 were produced
  std::size_t oldSize = buf.size();
  buf.resize_and_overwrite(oldSize + 6, [oldSize](std::byte *base, std::size_t) {
    // write 4 new bytes
    base[oldSize + 0] = std::byte{0xAA};
    base[oldSize + 1] = std::byte{0xBB};
    base[oldSize + 2] = std::byte{0xCC};
    base[oldSize + 3] = std::byte{0xDD};
    return oldSize + 4;  // only 4 valid
  });
  ASSERT_EQ(buf.size(), oldSize + 4);
  ASSERT_EQ(static_cast<unsigned>(buf[oldSize + 0]), 0xAAU);
  ASSERT_EQ(static_cast<unsigned>(buf[oldSize + 1]), 0xBBU);
  ASSERT_EQ(static_cast<unsigned>(buf[oldSize + 2]), 0xCCU);
  ASSERT_EQ(static_cast<unsigned>(buf[oldSize + 3]), 0xDDU);

  // Shrink (truncate) to 5
  buf.resize_and_overwrite(5, [](std::byte * /*base*/, std::size_t n) { return n; });
  ASSERT_EQ(buf.size(), 5U);
}

TEST(RawBytesResizeAndOverwrite, NoOpKeepsSizeWhenReturningOld) {
  RawBytes buf;
  buf.resize_and_overwrite(100, []([[maybe_unused]] std::byte *base, [[maybe_unused]] std::size_t n) {
    // pretend we only produced 0 bytes
    return static_cast<std::size_t>(0);
  });
  ASSERT_EQ(buf.size(), 0U);
}

TEST(RawBytesResizeAndOverwrite, ReserveAndPartialFill) {
  RawBytes buf;
  // Reserve space for 1024 but only fill 100
  buf.resize_and_overwrite(1024, [](std::byte *base, [[maybe_unused]] std::size_t n) {
    constexpr std::size_t produced = 100;
    for (std::size_t i = 0; i < produced; ++i) {
      base[i] = std::byte{0x7F};
    }
    return produced;
  });
  ASSERT_EQ(buf.size(), 100U);
  for (auto &byte : buf) {
    ASSERT_EQ(static_cast<unsigned>(byte), 0x7FU);
  }
}

// ---------------- Copy Constructor / Copy Assignment Tests ----------------

TEST(RawBytesCopy, CopyConstructorNonEmpty) {
  std::string payload = "CopyConstructorData";  // 19 bytes
  RawBytes src(reinterpret_cast<const std::byte *>(payload.data()),
               reinterpret_cast<const std::byte *>(payload.data()) + payload.size());
  ASSERT_EQ(src.size(), payload.size());
  auto srcCap = src.capacity();
  RawBytes dst(src);  // invoke copy ctor
  EXPECT_EQ(dst.size(), src.size());
  EXPECT_EQ(dst.capacity(), srcCap);  // copy ctor mirrors capacity
  EXPECT_TRUE(std::equal(dst.begin(), dst.end(), src.begin()));
}

TEST(RawBytesCopy, CopyConstructorEmpty) {
  RawBytes empty;
  RawBytes dst(empty);
  EXPECT_EQ(dst.size(), 0U);
  EXPECT_EQ(dst.capacity(), empty.capacity());
  EXPECT_TRUE(dst.begin() == dst.end());
}

TEST(RawBytesCopy, CopyAssignmentGrowCapacity) {
  // Source with some pre-reserved capacity
  std::string payload = std::string(64, 'A');
  RawBytes src(reinterpret_cast<const std::byte *>(payload.data()),
               reinterpret_cast<const std::byte *>(payload.data()) + payload.size());
  // Destination with smaller capacity and different content
  std::string small = "xx";  // capacity 2 (or small) – definitely < src.capacity()
  RawBytes dst(reinterpret_cast<const std::byte *>(small.data()),
               reinterpret_cast<const std::byte *>(small.data()) + small.size());
  auto oldCap = dst.capacity();
  dst = src;  // copy assignment
  EXPECT_EQ(dst.size(), src.size());
  EXPECT_GT(dst.capacity(), oldCap);  // capacity should have grown
  EXPECT_GE(dst.capacity(), dst.size());
  EXPECT_TRUE(std::equal(dst.begin(), dst.end(), src.begin()));
}

TEST(RawBytesCopy, CopyAssignmentNoCapacityGrowthWhenSufficient) {
  // Create destination with large capacity by reserving then shrinking size via resize_and_overwrite
  RawBytes dst;  // start empty
  dst.resize_and_overwrite(128, [](std::byte *base, std::size_t /*n*/) {
    // just fill first 10 bytes, return 10
    for (std::size_t i = 0; i < 10; ++i) {
      base[i] = std::byte{'X'};
    }
    return static_cast<std::size_t>(10);
  });
  auto largeCap = dst.capacity();
  // Source smaller than destination capacity
  std::string payload = std::string(16, 'B');
  RawBytes src(reinterpret_cast<const std::byte *>(payload.data()),
               reinterpret_cast<const std::byte *>(payload.data()) + payload.size());
  ASSERT_LT(src.capacity(), largeCap);
  dst = src;  // should NOT grow capacity nor shrink
  EXPECT_EQ(dst.size(), src.size());
  EXPECT_EQ(dst.capacity(), largeCap);  // capacity unchanged
  EXPECT_TRUE(std::equal(dst.begin(), dst.end(), src.begin()));
}

TEST(RawBytesCopy, CopyAssignmentFromEmpty) {
  std::string payload = std::string(32, 'Z');
  RawBytes src(reinterpret_cast<const std::byte *>(payload.data()),
               reinterpret_cast<const std::byte *>(payload.data()) + payload.size());
  RawBytes empty;  // empty source
  // Assign empty -> non-empty
  src = empty;
  EXPECT_EQ(src.size(), 0U);
  EXPECT_GE(src.capacity(), 0U);  // capacity retained (no shrink policy)
}

TEST(RawBytesCopy, CopyAssignmentIntoEmptyDestination) {
  RawBytes dst;  // empty with capacity 0
  std::string payload = std::string(48, 'Q');
  RawBytes src(reinterpret_cast<const std::byte *>(payload.data()),
               reinterpret_cast<const std::byte *>(payload.data()) + payload.size());
  dst = src;  // copy into empty destination
  EXPECT_EQ(dst.size(), src.size());
  EXPECT_GE(dst.capacity(), dst.size());
  // For assignment implementation, capacity may differ from srcCap; ensure content correct.
  EXPECT_TRUE(std::equal(dst.begin(), dst.end(), src.begin()));
}

TEST(RawBytesCopy, SelfAssignmentNoChange) {
  std::string payload = "SelfAssign";
  RawBytes buf(reinterpret_cast<const std::byte *>(payload.data()),
               reinterpret_cast<const std::byte *>(payload.data()) + payload.size());
  auto oldPtr = buf.data();
  auto oldCap = buf.capacity();
  auto oldSize = buf.size();
  // Intentional self-assignment: use reference alias to silence self-assignment warning tools while
  // still exercising the code path.
  RawBytes &alias = buf;
  buf = alias;
  EXPECT_EQ(buf.data(), oldPtr);
  EXPECT_EQ(buf.capacity(), oldCap);
  EXPECT_EQ(buf.size(), oldSize);
  EXPECT_TRUE(std::equal(buf.begin(), buf.end(), reinterpret_cast<const std::byte *>(payload.data())));
}

}  // namespace aeronet