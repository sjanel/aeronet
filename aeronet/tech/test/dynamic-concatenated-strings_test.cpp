
#include "aeronet/dynamic-concatenated-strings.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace aeronet {

namespace {
constexpr char kHeaderSep[] = ", ";
constexpr char kCRLF[] = "\r\n";
constexpr char kComma[] = ",";
constexpr char kNullCharSep[] = {'\0'};

using TestTypeCommaSpace32 = DynamicConcatenatedStrings<kHeaderSep, uint32_t>;
using TestTypeCommaSpace64 = DynamicConcatenatedStrings<kHeaderSep, uint64_t>;
using TestTypeCRLF32 = DynamicConcatenatedStrings<kCRLF, uint32_t>;
using TestTypeComma32 = DynamicConcatenatedStrings<kComma, uint32_t>;
using TestTypeNull32 = DynamicConcatenatedStrings<kNullCharSep, uint32_t>;
using TestTypeNull64 = DynamicConcatenatedStrings<kNullCharSep, uint64_t>;
}  // namespace

template <typename T>
class DynamicConcatenatedStringsTest : public ::testing::Test {
 public:
  using List = typename std::list<T>;
};

using MyTypes = ::testing::Types<TestTypeCommaSpace32, TestTypeCommaSpace64, TestTypeCRLF32, TestTypeComma32,
                                 TestTypeNull32, TestTypeNull64>;
TYPED_TEST_SUITE(DynamicConcatenatedStringsTest, MyTypes, );

TYPED_TEST(DynamicConcatenatedStringsTest, DefaultConstructor) {
  TypeParam pool;
  EXPECT_TRUE(pool.empty());
  EXPECT_EQ(pool.nbConcatenatedStrings(), 0U);
  EXPECT_EQ(pool.fullSize(), 0U);
  EXPECT_EQ(pool.fullSizeWithLastSep(), 0U);
  EXPECT_EQ(pool.internalBufferCapacity(), 0U);
  EXPECT_EQ(pool.fullString(), std::string_view{});
  EXPECT_EQ(pool.fullStringWithLastSep(), std::string_view{});
  EXPECT_EQ(pool.begin(), pool.end());
}

TYPED_TEST(DynamicConcatenatedStringsTest, AppendAndFullStringWithSep) {
  // instantiate with comma+space separator via string literal NTTP defined in a helper
  TypeParam pool;
  EXPECT_TRUE(pool.empty());
  pool.append("one");
  pool.append("two");
  pool.append("three");
  EXPECT_FALSE(pool.empty());
  EXPECT_EQ(pool.nbConcatenatedStrings(), 3U);
  EXPECT_EQ(pool.fullSize(), pool.fullString().size());
  auto full = pool.fullString();
  std::string expected = "one";
  expected.append(TypeParam::kSep);
  expected.append("two");
  expected.append(TypeParam::kSep);
  expected.append("three");
  EXPECT_EQ(full, std::string_view(expected));
}

TYPED_TEST(DynamicConcatenatedStringsTest, AppendTooLongPart) {
  if constexpr (sizeof(typename TypeParam::size_type) == sizeof(uint32_t)) {
    TypeParam pool;
    char ch{};

    std::string_view longPart(&ch, std::numeric_limits<uint32_t>::max());
    EXPECT_THROW(pool.append(longPart), std::overflow_error);
  }
}

TYPED_TEST(DynamicConcatenatedStringsTest, Contains) {
  TypeParam pool;
  EXPECT_FALSE(pool.contains("anything"));
  pool.append("AbC");
  pool.append("DeF");
  EXPECT_EQ(pool.nbConcatenatedStrings(), 2U);
  EXPECT_TRUE(pool.contains("AbC"));
  EXPECT_FALSE(pool.contains("abc"));
  EXPECT_FALSE(pool.contains("abcd"));
  EXPECT_FALSE(pool.contains("ab"));
  EXPECT_FALSE(pool.contains("bC"));
  EXPECT_TRUE(pool.contains("DeF"));
  EXPECT_FALSE(pool.contains("eF"));
  EXPECT_FALSE(pool.contains("DEF"));
  EXPECT_FALSE(pool.contains("De"));
  EXPECT_FALSE(pool.contains("DeFG"));
  EXPECT_FALSE(pool.contains("ghi"));
}

TYPED_TEST(DynamicConcatenatedStringsTest, ContainsCaseInsensitive) {
  TypeParam pool;
  pool.append("AbC");
  pool.append("DeF");
  EXPECT_EQ(pool.nbConcatenatedStrings(), 2U);
  EXPECT_TRUE(pool.containsCI("AbC"));
  EXPECT_TRUE(pool.containsCI("abc"));
  EXPECT_FALSE(pool.containsCI("abcd"));
  EXPECT_FALSE(pool.containsCI("ab"));
  EXPECT_FALSE(pool.containsCI("bC"));
  EXPECT_TRUE(pool.containsCI("DeF"));
  EXPECT_TRUE(pool.containsCI("DEF"));
  EXPECT_TRUE(pool.containsCI("DEF"));
  EXPECT_FALSE(pool.containsCI("eF"));
  EXPECT_FALSE(pool.containsCI("De"));
  EXPECT_FALSE(pool.containsCI("DeFG"));
  EXPECT_FALSE(pool.containsCI("ghi"));
}

TYPED_TEST(DynamicConcatenatedStringsTest, IteratorEmptyAndSingle) {
  TypeParam pool;
  // empty
  size_t count = 0;
  for (auto partView : pool) {
    (void)partView;
    ++count;
  }
  EXPECT_EQ(count, 0U);

  pool.append("solo");
  auto it = pool.begin();
  auto end = pool.end();
  EXPECT_NE(it, end);
  EXPECT_EQ(*it, std::string_view("solo"));
  ++it;
  EXPECT_EQ(it, end);
}

TYPED_TEST(DynamicConcatenatedStringsTest, IteratorMultipleParts) {
  TypeParam pool;
  pool.append("one");
  pool.append("two");
  pool.append("three");

  std::vector<std::string_view> parts;
  for (auto partView : pool) {
    parts.emplace_back(partView);
  }

  ASSERT_EQ(parts.size(), 3U);
  EXPECT_EQ(parts[0], std::string_view("one"));
  EXPECT_EQ(parts[1], std::string_view("two"));
  EXPECT_EQ(parts[2], std::string_view("three"));
}

TYPED_TEST(DynamicConcatenatedStringsTest, IteratorCaseInsensitive) {
  TypeParam pool;
  pool.append("AbC");
  pool.append("DeF");

  std::vector<std::string_view> parts;
  for (auto partView : pool) {
    parts.emplace_back(partView);
  }

  ASSERT_EQ(parts.size(), 2U);
  // iterator yields raw parts; contains uses case-insensitive compare
  EXPECT_EQ(parts[0], std::string_view("AbC"));
  EXPECT_EQ(parts[1], std::string_view("DeF"));
  EXPECT_FALSE(pool.contains("abc"));
  EXPECT_TRUE(pool.containsCI("abc"));
}

TYPED_TEST(DynamicConcatenatedStringsTest, FullString) {
  TypeParam pool;
  EXPECT_TRUE(pool.empty());
  pool.append("one");
  pool.append("two");
  pool.append("three");
  EXPECT_FALSE(pool.empty());
  EXPECT_EQ(pool.nbConcatenatedStrings(), 3U);
  auto full = pool.fullString();
  std::string expected = "one";
  expected.append(TypeParam::kSep);
  expected.append("two");
  expected.append(TypeParam::kSep);
  expected.append("three");
  EXPECT_EQ(full, expected);
}

TYPED_TEST(DynamicConcatenatedStringsTest, FullStringWithLastSep) {
  TypeParam pool;
  EXPECT_TRUE(pool.empty());
  pool.append("one");
  pool.append("two");
  pool.append("three");
  EXPECT_FALSE(pool.empty());
  EXPECT_EQ(pool.nbConcatenatedStrings(), 3U);
  auto full = pool.fullStringWithLastSep();
  std::string expected = "one";
  expected.append(TypeParam::kSep);
  expected.append("two");
  expected.append(TypeParam::kSep);
  expected.append("three");
  expected.append(TypeParam::kSep);
  EXPECT_EQ(full, expected);
}

TYPED_TEST(DynamicConcatenatedStringsTest, FullSize) {
  TypeParam pool;
  EXPECT_TRUE(pool.empty());
  pool.append("one");
  pool.append("two");
  pool.append("three");
  EXPECT_FALSE(pool.empty());
  EXPECT_EQ(pool.nbConcatenatedStrings(), 3U);
  EXPECT_EQ(pool.fullSize(), std::string_view("one").size() + std::string_view("two").size() +
                                 std::string_view("three").size() + (2UL * TypeParam::kSep.size()));
  EXPECT_EQ(pool.fullSizeWithLastSep(), std::string_view("one").size() + std::string_view("two").size() +
                                            std::string_view("three").size() + (3UL * TypeParam::kSep.size()));
}

TYPED_TEST(DynamicConcatenatedStringsTest, SizeEmptySingleMultipleClear) {
  TypeParam pool;
  using size_type_t = typename TypeParam::size_type;
  EXPECT_EQ(pool.nbConcatenatedStrings(), size_type_t{0});

  pool.append("one");
  EXPECT_EQ(pool.nbConcatenatedStrings(), size_type_t{1});

  pool.append("two");
  pool.append("three");
  EXPECT_EQ(pool.nbConcatenatedStrings(), size_type_t{3});
  EXPECT_GE(pool.internalBufferCapacity(), pool.fullSizeWithLastSep());

  pool.clear();
  EXPECT_EQ(pool.nbConcatenatedStrings(), size_type_t{0});
  EXPECT_TRUE(pool.empty());
  EXPECT_EQ(pool.fullSize(), 0U);
  EXPECT_EQ(pool.fullSizeWithLastSep(), 0U);
  EXPECT_EQ(pool.fullString(), std::string_view{});
  EXPECT_EQ(pool.fullStringWithLastSep(), std::string_view{});
  EXPECT_EQ(pool.begin(), pool.end());
  EXPECT_GT(pool.internalBufferCapacity(), 0U);  // capacity remains after clear
}

TYPED_TEST(DynamicConcatenatedStringsTest, EqualityOperator) {
  TypeParam pool1;
  TypeParam pool2;
  EXPECT_EQ(pool1, pool2);

  pool1.append("one");
  EXPECT_NE(pool1, pool2);

  pool2.append("one");
  EXPECT_EQ(pool1, pool2);

  pool1.append("two");
  pool1.append("three");
  pool2.append("two");
  pool2.append("three");
  EXPECT_EQ(pool1, pool2);

  pool2.append("four");
  EXPECT_NE(pool1, pool2);
}

}  // namespace aeronet