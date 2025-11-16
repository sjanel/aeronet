
#include "aeronet/dynamic-concatenated-strings.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace aeronet {

TEST(DynamicConcatenatedStringsTest, AppendAndFullStringWithSep) {
  // instantiate with comma+space separator via string literal NTTP defined in a helper
  static constexpr char sep[] = ", ";
  DynamicConcatenatedStrings<sep, false> pool;
  EXPECT_TRUE(pool.empty());
  pool.append("one");
  pool.append("two");
  pool.append("three");
  EXPECT_FALSE(pool.empty());
  EXPECT_EQ(pool.size(), 3U);
  EXPECT_EQ(pool.fullString(), std::string_view("one, two, three"));
  EXPECT_EQ(pool.fullSize(), pool.fullString().size());
}

TEST(DynamicConcatenatedStringsTest, ContainsCaseInsensitive) {
  static constexpr char sep[] = ", ";
  DynamicConcatenatedStrings<sep, true> pool;
  pool.append("AbC");
  pool.append("DeF");
  EXPECT_EQ(pool.size(), 2U);
  EXPECT_TRUE(pool.contains("abc"));
  EXPECT_TRUE(pool.contains("DEF"));
  EXPECT_FALSE(pool.contains("ghi"));
}

TEST(DynamicConcatenatedStringsTest, NoSepModeBehavesLikeBuffer) {
  // create a no-sep mode by passing a string literal starting with '\0'
  static constexpr char nos[] = "\0";
  DynamicConcatenatedStrings<nos, false> pool;
  pool.append("x");
  pool.append("y");
  // Implementation currently stores a null separator when Sep=="\0", so the full string
  // contains an embedded NUL between entries.
  std::string expected;
  expected.push_back('x');
  expected.push_back('\0');
  expected.push_back('y');
  EXPECT_EQ(pool.fullString(), std::string_view(expected));
  EXPECT_EQ(pool.fullSize(), expected.size());
}

TEST(DynamicConcatenatedStringsTest, NoSepKeepLastSepIncludesNul) {
  static constexpr char nos[] = "\0";
  DynamicConcatenatedStrings<nos, false> pool;
  pool.append("x");
  pool.append("y");
  // when asked to keep the last separator, the returned buffer includes the trailing NUL
  auto fullKeep = pool.fullStringWithLastSep();
  // build expected: 'x' '\0' 'y' '\0'
  std::string expected;
  expected.push_back('x');
  expected.push_back('\0');
  expected.push_back('y');
  expected.push_back('\0');
  EXPECT_EQ(fullKeep, std::string_view(expected));
  EXPECT_EQ(pool.fullSizeWithLastSep(), expected.size());
}

TEST(DynamicConcatenatedStringsTest, IteratorEmptyAndSingle) {
  static constexpr char sep[] = ", ";
  DynamicConcatenatedStrings<sep, false> pool;
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

TEST(DynamicConcatenatedStringsTest, IteratorMultipleParts) {
  static constexpr char sep[] = ", ";
  DynamicConcatenatedStrings<sep, false> pool;
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

TEST(DynamicConcatenatedStringsTest, IteratorCaseInsensitive) {
  static constexpr char sep[] = ", ";
  DynamicConcatenatedStrings<sep, true> pool;
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
  EXPECT_TRUE(pool.contains("abc"));
}

TEST(DynamicConcatenatedStringsTest, SizeEmptySingleMultipleClear) {
  static constexpr char sep[] = ", ";
  DynamicConcatenatedStrings<sep, false> pool;
  using size_type_t = typename DynamicConcatenatedStrings<sep, false>::size_type;
  EXPECT_EQ(pool.size(), size_type_t{0});

  pool.append("one");
  EXPECT_EQ(pool.size(), size_type_t{1});

  pool.append("two");
  pool.append("three");
  EXPECT_EQ(pool.size(), size_type_t{3});

  pool.clear();
  EXPECT_EQ(pool.size(), size_type_t{0});
}

TEST(DynamicConcatenatedStringsTest, SizeNoSepMode) {
  static constexpr char nos[] = "\0";
  DynamicConcatenatedStrings<nos, false> pool;
  using nos_size_type_t = typename DynamicConcatenatedStrings<nos, false>::size_type;
  EXPECT_EQ(pool.size(), nos_size_type_t{0});
  pool.append("a");
  EXPECT_EQ(pool.size(), nos_size_type_t{1});
  pool.append("b");
  EXPECT_EQ(pool.size(), nos_size_type_t{2});
}

}  // namespace aeronet