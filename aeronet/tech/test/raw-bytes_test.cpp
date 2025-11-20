#include "aeronet/raw-bytes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/config.hpp"
#include "aeronet/internal/raw-bytes-base.hpp"
#include "aeronet/vector.hpp"

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

TEST(RawBytesTest, DefaultConstructor) {
  RawBytes buf;
  EXPECT_EQ(buf.size(), 0);
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.data(), nullptr);
  EXPECT_EQ(buf.begin(), buf.end());
  EXPECT_EQ(buf.data(), buf.data() + buf.size());
}

TEST(RawBytesTest, ConstructorZeroCapacity) {
  RawBytes buf(0);
  EXPECT_EQ(buf.size(), 0);
  EXPECT_TRUE(buf.empty());
  EXPECT_NE(buf.data(), nullptr);  // malloc(0) may return non-null
  EXPECT_EQ(buf.begin(), buf.end());
  EXPECT_EQ(buf.data(), buf.data() + buf.size());
}

TEST(RawBytesTest, RandomAccessIteratorConstructor) {
  std::string text = "Hello";
  RawBytes buf(reinterpret_cast<const std::byte *>(text.data()),
               reinterpret_cast<const std::byte *>(text.data()) + text.size());
  EXPECT_EQ(buf.size(), text.size());
  BytesView bufView(buf);
  auto textView = toBytes(text);
  EXPECT_TRUE(std::equal(bufView.data(), bufView.data() + bufView.size(), textView.data()));
}

TEST(RawBytesTest, MoveConstructor) {
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

TEST(RawBytesTest, MoveAssignment) {
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

TEST(RawBytesTest, RangeForLoop) {
  std::string text = "range";
  RawBytes buf(reinterpret_cast<const std::byte *>(text.data()),
               reinterpret_cast<const std::byte *>(text.data()) + text.size());
  std::string collected;
  for (auto ch : buf) {
    collected.push_back(static_cast<char>(ch));
  }
  EXPECT_EQ(collected, text);
}

TEST(RawBytesTest, RangesAlgorithmsWork) {
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

TEST(RawBytesTest, GuardAgainstSmallSizeTypeOverflow) {
  RawBytesBase<char, std::string_view, uint8_t> smallBuffer;

  smallBuffer.append(std::string(100U, 'A'));  // OK
  smallBuffer.append(std::string(100U, 'B'));  // OK
  EXPECT_THROW(smallBuffer.append(std::string(100U, 'C')), std::bad_alloc);
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

TEST(RawBytesEqual, EqualityOperatorNominal) {
  std::string payload1 = "EqualTestData";
  RawBytes buf1(reinterpret_cast<const std::byte *>(payload1.data()),
                reinterpret_cast<const std::byte *>(payload1.data()) + payload1.size());
  RawBytes buf2(reinterpret_cast<const std::byte *>(payload1.data()),
                reinterpret_cast<const std::byte *>(payload1.data()) + payload1.size());
  EXPECT_EQ(buf1, buf2);
  EXPECT_EQ(buf2, buf1);

  // Different size
  RawBytes buf3(reinterpret_cast<const std::byte *>(payload1.data()),
                reinterpret_cast<const std::byte *>(payload1.data()) + payload1.size() - 2);
  EXPECT_NE(buf1, buf3);
  EXPECT_NE(buf3, buf1);

  // Different content
  std::string payload2 = "EqualTestDataX";
  RawBytes buf4(reinterpret_cast<const std::byte *>(payload2.data()),
                reinterpret_cast<const std::byte *>(payload2.data()) + payload2.size());
  EXPECT_NE(buf1, buf4);
  EXPECT_NE(buf4, buf1);

  // Different contentSameSize
  std::string payload3 = "EqualTestDita";
  RawBytes buf5(reinterpret_cast<const std::byte *>(payload3.data()),
                reinterpret_cast<const std::byte *>(payload3.data()) + payload3.size());
  EXPECT_NE(buf1, buf5);
  EXPECT_NE(buf5, buf1);
}

TEST(RawBytesEqual, EqualityEmpty) {
  RawBytes buf1;
  RawBytes buf2(std::span<const std::byte>{});
  EXPECT_EQ(buf1, buf2);

  buf1.push_back(static_cast<std::byte>('a'));
  EXPECT_NE(buf1, buf2);
  EXPECT_NE(buf2, buf1);
}

TEST(RawBytes, CopyFromEmpty) {
  RawBytes buf(10);
  RawBytes buf2{std::span<const std::byte>{}};
  buf = buf2;
  EXPECT_EQ(buf.size(), 0U);
  EXPECT_EQ(buf.capacity(), 10);
}

TEST(RawBytes, InvalidSetSize) {
  RawBytes buf(42);

  EXPECT_DEBUG_DEATH(buf.setSize(43), "");  // larger than capacity
}

TEST(RawBytes, AppendEmpty) {
  RawBytes buf(10);

  auto ptr = buf.data();
  buf.append(ptr, ptr);  // OK: zero-length append
  EXPECT_EQ(buf.size(), 0U);
}

TEST(RawBytes, UnreasonableReserve) {
  using TinyRawBytes = RawBytesBase<char, std::string_view, std::uint8_t>;
  TinyRawBytes buf(150);

  buf.append(std::string(150, 'A'));  // OK
  EXPECT_THROW(buf.reserveExponential(151), std::bad_alloc);
  EXPECT_THROW(TinyRawBytes(std::string(300, 'B')), std::length_error);
}

#ifndef NDEBUG
TEST(RawBytes, Asserts) {
  RawBytes buf(10);

  EXPECT_DEBUG_DEATH(buf.erase_front(1), "");
  EXPECT_DEBUG_DEATH(buf.setSize(11), "");
  EXPECT_DEBUG_DEATH(buf.addSize(11), "");
}
#endif

TEST(RawBytes, Assign) {
  RawBytes buf;

  buf.assign(reinterpret_cast<const std::byte *>("abcdef"), 6);
  EXPECT_EQ(buf.size(), 6U);
  EXPECT_EQ(std::memcmp(buf.data(), "abcdef", 6), 0);

  buf.assign(std::span<const std::byte>(reinterpret_cast<const std::byte *>("ghijkl"), 6));
  EXPECT_EQ(buf.size(), 6U);
  EXPECT_EQ(std::memcmp(buf.data(), "ghijkl", 6), 0);

  buf.assign(reinterpret_cast<const std::byte *>("mnopqr"), reinterpret_cast<const std::byte *>("mnopqr") + 6);
  EXPECT_EQ(buf.size(), 6U);
  EXPECT_EQ(std::memcmp(buf.data(), "mnopqr", 6), 0);
}

TEST(RawBytes, UnreasanableMalloc) {
  // AddressSanitizer imposes a maximum allocation size and will abort the
  // process instead of allowing malloc to return null for extremely large
  // allocation requests. In that environment the test cannot observe
  // std::bad_alloc. Detect ASan at compile time and skip the test with a
  // helpful message.
#ifdef AERONET_ASAN_ENABLED
  GTEST_SKIP() << "ASan blocks huge allocations. To run this test, either build without ASan or run the test with\n"
               << "ASAN_OPTIONS=allocator_may_return_null=1 ./build/aeronet/tech/raw-bytes_test "
                  "--gtest_filter=RawBytes.UnreasanableMalloc";
#else
  EXPECT_THROW(RawBytes(1UL << 45), std::bad_alloc);
  RawBytes buf(10);
  EXPECT_THROW(buf.reserve(1UL << 45), std::bad_alloc);
#endif
}

}  // namespace aeronet