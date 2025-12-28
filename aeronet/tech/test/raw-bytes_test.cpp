#include "aeronet/raw-bytes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/raw-chars.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

template <typename T>
class RawBaseTest : public ::testing::Test {
 public:
  using List = typename std::list<T>;
};

using MyTypes = ::testing::Types<RawBytes32, RawChars32, RawBytes, RawChars>;
TYPED_TEST_SUITE(RawBaseTest, MyTypes, );

TYPED_TEST(RawBaseTest, DefaultConstructor) {
  using RawT = TypeParam;
  RawT buf;
  EXPECT_EQ(buf.size(), 0);
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.data(), nullptr);
  EXPECT_EQ(buf.begin(), buf.end());
  EXPECT_EQ(buf.data(), buf.data() + buf.size());
}

TYPED_TEST(RawBaseTest, ConstructorZeroCapacity) {
  using RawT = TypeParam;
  RawT buf(0);
  EXPECT_EQ(buf.size(), 0);
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.begin(), buf.end());
}

TYPED_TEST(RawBaseTest, RandomAccessIteratorConstructor) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;
  using ViewType = typename RawT::view_type;
  using SizeType = typename RawT::size_type;

  std::string text = "Hello";
  RawT buf(reinterpret_cast<const Type *>(text.data()), static_cast<SizeType>(text.size()));
  EXPECT_EQ(buf.size(), text.size());
  ViewType bufView(buf);
  EXPECT_TRUE(std::equal(bufView.data(), bufView.data() + bufView.size(), reinterpret_cast<const Type *>(text.data())));
}

TYPED_TEST(RawBaseTest, MoveConstructor) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;
  using ViewType = typename RawT::view_type;
  using SizeType = typename RawT::size_type;

  std::string_view data = "move-me";
  RawT buf1(reinterpret_cast<const Type *>(data.data()), static_cast<SizeType>(data.size()));
  auto *oldPtr = buf1.data();

  RawT buf2(std::move(buf1));
  EXPECT_EQ(buf2.data(), oldPtr);

  ViewType buf2View(buf2);
  ViewType dataView(reinterpret_cast<const Type *>(data.data()), data.size());

  EXPECT_EQ(buf2.size(), data.size());
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  EXPECT_TRUE(std::equal(buf2View.data(), buf2View.data() + buf2View.size(), dataView.data()));
}

TYPED_TEST(RawBaseTest, MoveAssignment) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;
  using ViewType = typename RawT::view_type;
  using SizeType = typename RawT::size_type;

  std::string_view data = "move-me";
  RawT buf1(reinterpret_cast<const Type *>(data.data()), static_cast<SizeType>(data.size()));
  auto *oldPtr = buf1.data();

  RawT buf2;
  buf2 = std::move(buf1);
  EXPECT_EQ(buf2.data(), oldPtr);

  ViewType buf2View(buf2);
  ViewType dataView(reinterpret_cast<const Type *>(data.data()), data.size());

  EXPECT_EQ(buf2.size(), data.size());
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  EXPECT_TRUE(std::equal(buf2View.data(), buf2View.data() + buf2View.size(), dataView.data()));

  // self-move should do nothing
  auto &buf2Bis = buf2;
  buf2 = std::move(buf2Bis);
  EXPECT_EQ(buf2.data(), oldPtr);
  EXPECT_EQ(buf2.size(), data.size());
}

TYPED_TEST(RawBaseTest, CopyAssignment) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;
  using ViewType = typename RawT::view_type;
  using SizeType = typename RawT::size_type;

  std::string_view data = "copy-me";
  RawT buf1(reinterpret_cast<const Type *>(data.data()), static_cast<SizeType>(data.size()));

  RawT buf2;
  buf2 = buf1;

  ViewType buf2View(buf2);
  ViewType dataView(reinterpret_cast<const Type *>(data.data()), data.size());

  EXPECT_EQ(buf2.size(), data.size());
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  EXPECT_TRUE(std::equal(buf2View.data(), buf2View.data() + buf2View.size(), dataView.data()));

  // self-copy should do nothing
  auto &buf2Bis = buf2;
  buf2 = buf2Bis;
  EXPECT_EQ(buf2.size(), data.size());
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  EXPECT_TRUE(std::equal(buf2View.data(), buf2View.data() + buf2View.size(), dataView.data()));
}

TYPED_TEST(RawBaseTest, RangeForLoop) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;
  using SizeType = typename RawT::size_type;

  std::string_view text = "range";
  RawT buf(reinterpret_cast<const Type *>(text.data()), static_cast<SizeType>(text.size()));
  std::string collected;
  for (auto ch : buf) {
    collected.push_back(static_cast<char>(ch));
  }
  EXPECT_EQ(collected, text);
}

TYPED_TEST(RawBaseTest, RangesAlgorithmsWork) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;
  using VType = vector<Type>;
  using VTypeSize = VType::size_type;

  VType text({Type('x'), Type('y'), Type('z')});
  RawT buf(text.data(), text.size());

  // ranges::equal
  EXPECT_TRUE(std::ranges::equal(buf, text));
  // ranges::copy
  ASSERT_LE(buf.size(), static_cast<std::size_t>(std::numeric_limits<VTypeSize>::max()));
  VType copied(static_cast<VTypeSize>(buf.size()));
  std::ranges::copy(buf, copied.begin());
  std::ranges::copy(buf, copied.begin());
  EXPECT_EQ(copied, text);
}

TYPED_TEST(RawBaseTest, GuardAgainstSmallSizeTypeOverflow) {
  using RawT = TypeParam;
  using SizeType = typename RawT::size_type;
  using Type = typename RawT::value_type;
  using ViewType = typename RawT::view_type;

  RawT smallBuffer;

  Type ch{};
  ViewType large(&ch, std::numeric_limits<uint32_t>::max() - 45UL);

  smallBuffer.append(ViewType{vector<Type>(150UL, Type{'A'})});  // OK
  if constexpr (sizeof(SizeType) < sizeof(std::size_t)) {
    EXPECT_THROW(smallBuffer.append(large), std::overflow_error);
  }
}

// ---------------- Copy Constructor / Copy Assignment Tests ----------------

TYPED_TEST(RawBaseTest, CopyConstructorNonEmpty) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;
  using SizeType = typename RawT::size_type;

  const char *payload = "CopyConstructorData";  // 19 bytes
  RawT src(reinterpret_cast<const Type *>(payload), static_cast<SizeType>(std::strlen(payload)));
  ASSERT_EQ(src.size(), strlen(payload));
  auto srcCap = src.capacity();
  RawT dst(src);  // invoke copy ctor
  EXPECT_EQ(dst.size(), src.size());
  EXPECT_EQ(dst.capacity(), srcCap);  // copy ctor mirrors capacity
  EXPECT_TRUE(std::equal(dst.begin(), dst.end(), src.begin()));
}

TYPED_TEST(RawBaseTest, CopyConstructorEmpty) {
  using RawT = TypeParam;

  RawT empty;
  RawT dst(empty);
  EXPECT_EQ(dst.size(), 0U);
  EXPECT_EQ(dst.capacity(), empty.capacity());
  EXPECT_TRUE(dst.begin() == dst.end());
}

TYPED_TEST(RawBaseTest, CopyAssignmentGrowCapacity) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;
  using SizeType = typename RawT::size_type;

  // Source with some pre-reserved capacity
  std::string payload(64, 'A');
  RawT src(reinterpret_cast<const Type *>(payload.data()), static_cast<SizeType>(payload.size()));
  // Destination with smaller capacity and different content
  std::string small = "xx";  // capacity 2 (or small) – definitely < src.capacity()
  RawT dst(reinterpret_cast<const Type *>(small.data()), static_cast<SizeType>(small.size()));
  auto oldCap = dst.capacity();
  dst = src;  // copy assignment
  EXPECT_EQ(dst.size(), src.size());
  EXPECT_GT(dst.capacity(), oldCap);  // capacity should have grown
  EXPECT_GE(dst.capacity(), dst.size());
  EXPECT_TRUE(std::equal(dst.begin(), dst.end(), src.begin()));
}

TYPED_TEST(RawBaseTest, CopyAssignmentFromEmpty) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;
  using SizeType = typename RawT::size_type;

  std::string payload = std::string(32, 'Z');
  RawT src(reinterpret_cast<const Type *>(payload.data()), static_cast<SizeType>(payload.size()));
  RawT empty;  // empty source
  // Assign empty -> non-empty
  src = empty;
  EXPECT_EQ(src.size(), 0U);
  EXPECT_GE(src.capacity(), 0U);  // capacity retained (no shrink policy)
}

TYPED_TEST(RawBaseTest, CopyAssignmentIntoEmptyDestination) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;
  using SizeType = typename RawT::size_type;

  RawT dst;  // empty with capacity 0
  std::string payload = std::string(48, 'Q');
  RawT src(reinterpret_cast<const Type *>(payload.data()), static_cast<SizeType>(payload.size()));
  dst = src;  // copy into empty destination
  EXPECT_EQ(dst.size(), src.size());
  EXPECT_GE(dst.capacity(), dst.size());
  // For assignment implementation, capacity may differ from srcCap; ensure content correct.
  EXPECT_TRUE(std::equal(dst.begin(), dst.end(), src.begin()));
}

TYPED_TEST(RawBaseTest, SelfAssignmentNoChange) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;
  using SizeType = typename RawT::size_type;

  std::string payload = "SelfAssign";
  RawT buf(reinterpret_cast<const Type *>(payload.data()), static_cast<SizeType>(payload.size()));
  auto oldPtr = buf.data();
  auto oldCap = buf.capacity();
  auto oldSize = buf.size();
  // Intentional self-assignment: use reference alias to silence self-assignment warning tools while
  // still exercising the code path.
  RawT &alias = buf;
  buf = alias;
  EXPECT_EQ(buf.data(), oldPtr);
  EXPECT_EQ(buf.capacity(), oldCap);
  EXPECT_EQ(buf.size(), oldSize);
  EXPECT_TRUE(std::equal(buf.begin(), buf.end(), reinterpret_cast<const Type *>(payload.data())));
}

TYPED_TEST(RawBaseTest, EqualityOperatorNominal) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;
  using SizeType = typename RawT::size_type;

  std::string payload1 = "EqualTestData";
  RawT buf1(reinterpret_cast<const Type *>(payload1.data()), static_cast<SizeType>(payload1.size()));
  RawT buf2(reinterpret_cast<const Type *>(payload1.data()), static_cast<SizeType>(payload1.size()));
  EXPECT_EQ(buf1, buf2);
  EXPECT_EQ(buf2, buf1);

  // Different size
  RawT buf3(reinterpret_cast<const Type *>(payload1.data()), static_cast<SizeType>(payload1.size() - 2));
  EXPECT_NE(buf1, buf3);
  EXPECT_NE(buf3, buf1);

  // Different content
  std::string payload2 = "EqualTestDataX";
  RawT buf4(reinterpret_cast<const Type *>(payload2.data()), static_cast<SizeType>(payload2.size()));
  EXPECT_NE(buf1, buf4);
  EXPECT_NE(buf4, buf1);

  // Different contentSameSize
  std::string payload3 = "EqualTestDita";
  RawT buf5(reinterpret_cast<const Type *>(payload3.data()), static_cast<SizeType>(payload3.size()));
  EXPECT_NE(buf1, buf5);
  EXPECT_NE(buf5, buf1);
}

TYPED_TEST(RawBaseTest, EqualityEmpty) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;
  using ViewType = typename RawT::view_type;

  RawT buf1;
  RawT buf2(ViewType{});
  EXPECT_EQ(buf1, buf2);

  buf1.push_back(static_cast<Type>('a'));
  EXPECT_NE(buf1, buf2);
  EXPECT_NE(buf2, buf1);
}

TYPED_TEST(RawBaseTest, CopyFromEmpty) {
  using RawT = TypeParam;
  using ViewType = typename RawT::view_type;

  RawT buf(10);
  RawT buf2{ViewType{}};
  buf = buf2;
  EXPECT_EQ(buf.size(), 0U);
  EXPECT_EQ(buf.capacity(), 10);
}

TYPED_TEST(RawBaseTest, UncheckedAppendAndOverflowCheck) {
  using RawT = RawChars32;
  using Type = typename RawT::value_type;
  using SizeType = typename RawT::size_type;

  RawT buf(10);
  std::string data = "1234567890";  // 10 bytes
  buf.unchecked_append(reinterpret_cast<const Type *>(data.data()), static_cast<SizeType>(data.size()));
  EXPECT_EQ(buf.size(), data.size());
  EXPECT_TRUE(std::equal(buf.begin(), buf.end(), reinterpret_cast<const Type *>(data.data())));

  EXPECT_THROW(buf.unchecked_append(reinterpret_cast<const Type *>("extra"), std::numeric_limits<SizeType>::max() - 5U),
               std::overflow_error);
}

TYPED_TEST(RawBaseTest, AppendEmpty) {
  using RawT = TypeParam;

  RawT buf(10);

  auto ptr = buf.data();
  buf.append(ptr, 0);  // OK: zero-length append
  EXPECT_EQ(buf.size(), 0U);

  buf.unchecked_append(ptr, 0);  // OK: zero-length unchecked append
  EXPECT_EQ(buf.size(), 0U);
}

TYPED_TEST(RawBaseTest, EraseFront) {
  using RawT = TypeParam;

  RawT buf(10);

  buf.append(reinterpret_cast<const typename RawT::value_type *>("abcdefghij"), 10);

  buf.erase_front(4);
  EXPECT_EQ(buf.size(), 6U);
  EXPECT_EQ(std::memcmp(buf.data(), "efghij", 6), 0);
  buf.erase_front(0);  // no-op
  EXPECT_EQ(buf.size(), 6U);
  EXPECT_EQ(std::memcmp(buf.data(), "efghij", 6), 0);
  buf.erase_front(6);
  EXPECT_EQ(buf.size(), 0U);
}

TYPED_TEST(RawBaseTest, EnsureAndOverflowCheck) {
  using RawT = TypeParam;
  using SizeType = typename RawT::size_type;

  RawT buf;

  buf.ensureAvailableCapacity(16);
  EXPECT_GE(buf.capacity(), 16U);

  buf.unchecked_append(reinterpret_cast<const typename RawT::value_type *>("1234567890"), 10U);

  if constexpr (sizeof(SizeType) < sizeof(uintmax_t)) {
    EXPECT_THROW(buf.ensureAvailableCapacity(std::numeric_limits<SizeType>::max() - 5U), std::overflow_error);
  }
}

TYPED_TEST(RawBaseTest, EnsureExponentialAndOverflowCheck) {
  using RawT = TypeParam;
  using SizeType = typename RawT::size_type;

  RawT buf;

  buf.ensureAvailableCapacityExponential(16);
  EXPECT_GE(buf.capacity(), 16U);

  buf.unchecked_append(reinterpret_cast<const typename RawT::value_type *>("1234567890"), 10U);

  if constexpr (sizeof(SizeType) < sizeof(uintmax_t)) {
    EXPECT_THROW(buf.ensureAvailableCapacityExponential(std::numeric_limits<SizeType>::max() - 5U),
                 std::overflow_error);
  }
}

TYPED_TEST(RawBaseTest, Assign) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;
  using ViewType = typename RawT::view_type;
  RawT buf;

  buf.assign(reinterpret_cast<const Type *>("abcdef"), 6);
  EXPECT_EQ(buf.size(), 6U);
  EXPECT_EQ(std::memcmp(buf.data(), "abcdef", 6), 0);

  buf.assign(ViewType(reinterpret_cast<const Type *>("ghijkl"), 6));
  EXPECT_EQ(buf.size(), 6U);
  EXPECT_EQ(std::memcmp(buf.data(), "ghijkl", 6), 0);

  buf.assign(reinterpret_cast<const Type *>("mnopqr"), 6);
  EXPECT_EQ(buf.size(), 6U);
  EXPECT_EQ(std::memcmp(buf.data(), "mnopqr", 6), 0);

  buf.assign(reinterpret_cast<const Type *>("abcdef"), 0);
  EXPECT_EQ(buf.size(), 0U);
}

TYPED_TEST(RawBaseTest, ContPointersConstructor) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;

  static constexpr char rawData[] = "constructor";
  RawT buf(reinterpret_cast<const Type *>(rawData), sizeof(rawData) - 1);
  EXPECT_EQ(buf.size(), sizeof(rawData) - 1);
  EXPECT_EQ(std::memcmp(buf.data(), rawData, sizeof(rawData) - 1), 0);

  buf = RawT(reinterpret_cast<const Type *>(rawData), 0);
  EXPECT_EQ(buf.size(), 0U);
}

TYPED_TEST(RawBaseTest, MallocFails) {
  if (!AERONET_WANT_MALLOC_OVERRIDES) {
    GTEST_SKIP() << "malloc overrides disabled on this toolchain; skipping";
  }
  using RawT = TypeParam;

  test::FailNextMalloc();
  EXPECT_THROW(RawT(16UL), std::bad_alloc);
  test::FailNextMalloc();
  EXPECT_NO_THROW(RawT(0UL));  // zero-size allocation should not fail
  RawT buf1(10);
  test::FailNextRealloc();
  EXPECT_THROW(buf1.reserve(32UL), std::bad_alloc);
}

TYPED_TEST(RawBaseTest, ShrinkToFit) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;

  RawT buf;

  EXPECT_EQ(buf.size(), 0U);
  EXPECT_EQ(buf.capacity(), 0U);
  buf.shrink_to_fit();
  EXPECT_EQ(buf.size(), 0U);
  EXPECT_EQ(buf.capacity(), 0U);

  buf.assign(reinterpret_cast<const Type *>("abcdefghij"), 10);

  buf.ensureAvailableCapacityExponential(100);

  auto oldCap = buf.capacity();
  EXPECT_GT(oldCap, 10U);

  buf.shrink_to_fit();
  EXPECT_EQ(buf.size(), 10U);
  EXPECT_EQ(buf.capacity(), 10U);
  EXPECT_EQ(std::memcmp(buf.data(), "abcdefghij", 10), 0);

  // Shrink when size == capacity is no-op
  buf.shrink_to_fit();
  EXPECT_EQ(buf.size(), 10U);
  EXPECT_EQ(buf.capacity(), 10U);
  EXPECT_EQ(std::memcmp(buf.data(), "abcdefghij", 10), 0);

  buf.clear();
  EXPECT_EQ(buf.size(), 0U);
  EXPECT_GT(buf.capacity(), 0);
  buf.shrink_to_fit();
  EXPECT_EQ(buf.size(), 0U);
  EXPECT_EQ(buf.capacity(), 0U);
}

TYPED_TEST(RawBaseTest, EraseFrontSetSizeAndAddSize) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;

  RawT buf;
  buf.assign(reinterpret_cast<const Type *>("abcdefgh"), 8);
  buf.erase_front(3);
  ASSERT_EQ(buf.size(), 5U);
  EXPECT_EQ(std::memcmp(buf.data(), "defgh", 5), 0);

  buf.ensureAvailableCapacityExponential(10);
  buf.setSize(2);
  EXPECT_EQ(buf.size(), 2U);

  const char tail[] = "XYZ";
  std::memcpy(buf.data() + buf.size(), tail, sizeof(tail) - 1);
  buf.addSize(3);
  EXPECT_EQ(buf.size(), 5U);
  EXPECT_EQ(std::memcmp(buf.data(), "deXYZ", 5), 0);
}

TYPED_TEST(RawBaseTest, SwapAndSpanConstructor) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;
  using ViewType = typename RawT::view_type;

  std::string_view initial = "payload";
  ViewType view(reinterpret_cast<const Type *>(initial.data()), initial.size());
  RawT fromSpan(view);

  RawT other(reinterpret_cast<const Type *>("swap"), 4);
  fromSpan.swap(other);

  EXPECT_EQ(fromSpan.size(), 4U);
  EXPECT_EQ(std::memcmp(fromSpan.data(), "swap", 4), 0);
  EXPECT_EQ(other.size(), view.size());
  EXPECT_EQ(std::memcmp(other.data(), "payload", view.size()), 0);
}

TYPED_TEST(RawBaseTest, SafeCastShouldCheckForOverflow) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;
  using ViewType = typename RawT::view_type;
  using SizeType = typename RawT::size_type;

  if constexpr (sizeof(SizeType) < sizeof(std::size_t)) {
    Type buf{};
    ViewType fakeData(&buf, static_cast<std::size_t>(std::numeric_limits<SizeType>::max() + 2ULL));

    EXPECT_THROW(RawT{fakeData}, std::overflow_error);
  }
}

TYPED_TEST(RawBaseTest, Swap) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;

  RawT buf1(reinterpret_cast<const Type *>("buffer1"), 7);
  RawT buf2(reinterpret_cast<const Type *>("buf2data"), 8);

  auto buf1DataPtr = buf1.data();
  auto buf1Size = buf1.size();
  auto buf1Cap = buf1.capacity();

  auto buf2DataPtr = buf2.data();
  auto buf2Size = buf2.size();
  auto buf2Cap = buf2.capacity();

  buf1.swap(buf2);

  EXPECT_EQ(buf1.data(), buf2DataPtr);
  EXPECT_EQ(buf1.size(), buf2Size);
  EXPECT_EQ(buf1.capacity(), buf2Cap);

  EXPECT_EQ(buf2.data(), buf1DataPtr);
  EXPECT_EQ(buf2.size(), buf1Size);
  EXPECT_EQ(buf2.capacity(), buf1Cap);

  RawT emptyBuf;
  buf1.swap(emptyBuf);
  EXPECT_EQ(buf1.size(), 0U);
  EXPECT_EQ(emptyBuf.data(), buf2DataPtr);
  EXPECT_EQ(emptyBuf.size(), buf2Size);
  EXPECT_EQ(emptyBuf.capacity(), buf2Cap);

  if (!AERONET_WANT_MALLOC_OVERRIDES) {
    GTEST_SKIP() << "malloc overrides disabled on this toolchain; skipping";
  }
  buf2.ensureAvailableCapacity(1024);
  test::FailNextRealloc();
  EXPECT_NO_THROW(buf2.shrink_to_fit());  // should not throw even if realloc fails
}

TYPED_TEST(RawBaseTest, EqualityCheck) {
  using RawT = TypeParam;
  using Type = typename RawT::value_type;

  RawT buf1(reinterpret_cast<const Type *>("testdata"), 8);
  RawT buf2(reinterpret_cast<const Type *>("testdata"), 8);
  RawT buf3(reinterpret_cast<const Type *>("otherdata"), 9);
  RawT buf4(reinterpret_cast<const Type *>("tesTdata"), 8);
  RawT buf5;
  RawT buf6(8U);

  EXPECT_EQ(buf1, buf1);
  EXPECT_EQ(buf1, buf2);
  EXPECT_NE(buf1, buf3);
  EXPECT_NE(buf1, buf4);
  EXPECT_NE(buf1, buf5);
  EXPECT_NE(buf1, buf6);

  EXPECT_EQ(buf2, buf1);
  EXPECT_EQ(buf2, buf2);
  EXPECT_NE(buf2, buf3);
  EXPECT_NE(buf2, buf4);
  EXPECT_NE(buf2, buf5);
  EXPECT_NE(buf2, buf6);

  EXPECT_NE(buf3, buf1);
  EXPECT_NE(buf3, buf2);
  EXPECT_EQ(buf3, buf3);
  EXPECT_NE(buf3, buf4);
  EXPECT_NE(buf3, buf5);
  EXPECT_NE(buf3, buf6);

  EXPECT_NE(buf4, buf1);
  EXPECT_NE(buf4, buf2);
  EXPECT_NE(buf4, buf3);
  EXPECT_EQ(buf4, buf4);
  EXPECT_NE(buf4, buf5);
  EXPECT_NE(buf4, buf6);

  EXPECT_NE(buf5, buf1);
  EXPECT_NE(buf5, buf2);
  EXPECT_NE(buf5, buf3);
  EXPECT_NE(buf5, buf4);
  EXPECT_EQ(buf5, buf5);
  EXPECT_EQ(buf5, buf6);

  EXPECT_NE(buf6, buf1);
  EXPECT_NE(buf6, buf2);
  EXPECT_NE(buf6, buf3);
  EXPECT_NE(buf6, buf4);
  EXPECT_EQ(buf6, buf5);
  EXPECT_EQ(buf6, buf6);
}

}  // namespace aeronet