
#include "dynamic-concatenated-strings.hpp"

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
  EXPECT_EQ(pool.size(), 2U);  // with no sep, size counts entries since append still increments
}

TEST(DynamicConcatenatedStringsTest, CaptureAndClear) {
  static constexpr char sep[] = "; ";
  DynamicConcatenatedStrings<sep, false> pool;
  pool.append("a");
  pool.append("b");
  auto captured = pool.captureFullString();
  EXPECT_EQ(std::string_view(captured.data(), captured.size()), std::string_view("a; b"));
  EXPECT_TRUE(pool.empty());
}

TEST(DynamicConcatenatedStringsTest, FullStringAndCaptureKeepLastSep) {
  static constexpr char sep[] = ", ";
  DynamicConcatenatedStrings<sep, false> pool;
  pool.append("one");
  pool.append("two");
  // keep the trailing separator
  auto fullKeep = pool.fullString(/*removeLastSep=*/false);
  EXPECT_EQ(fullKeep, std::string_view("one, two, "));
  EXPECT_EQ(pool.fullSize(/*removeLastSep=*/false), fullKeep.size());

  // capture without removing last sep should return the buffer with trailing sep
  auto captured = pool.captureFullString(/*removeLastSep=*/false);
  EXPECT_EQ(std::string_view(captured.data(), captured.size()), std::string_view("one, two, "));
}

TEST(DynamicConcatenatedStringsTest, NoSepKeepLastSepIncludesNul) {
  static constexpr char nos[] = "\0";
  DynamicConcatenatedStrings<nos, false> pool;
  pool.append("x");
  pool.append("y");
  // when asked to keep the last separator, the returned buffer includes the trailing NUL
  auto fullKeep = pool.fullString(/*removeLastSep=*/false);
  // build expected: 'x' '\0' 'y' '\0'
  std::string expected;
  expected.push_back('x');
  expected.push_back('\0');
  expected.push_back('y');
  expected.push_back('\0');
  EXPECT_EQ(fullKeep, std::string_view(expected));
  EXPECT_EQ(pool.fullSize(/*removeLastSep=*/false), expected.size());
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

}  // namespace aeronet