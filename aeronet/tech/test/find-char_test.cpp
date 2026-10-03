#include "aeronet/find-char.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace aeronet {

TEST(FindChar, FindsFirstOccurrenceOrReturnsLast) {
  std::string_view str = "a?b?c";
  const char* first = str.data();
  const char* last = first + str.size();
  EXPECT_EQ(FindChar(first, last, 'a'), first);
  EXPECT_EQ(FindChar(first, last, '?'), first + 1);
  EXPECT_EQ(FindChar(first, last, 'c'), last - 1);
  EXPECT_EQ(FindChar(first, last, 'z'), last);
  EXPECT_EQ(FindChar(first, first, 'a'), first);
  // only [first, last) is searched
  EXPECT_EQ(FindChar(first, first + 2, 'b'), first + 2);
}

TEST(FindChar, MutableRangeAndNonAsciiBytes) {
  std::string str = "caf\xC3\xA9";
  str.push_back('\0');
  char* first = str.data();
  char* last = first + str.size();
  char* pos = FindChar(first, last, '\xA9');
  ASSERT_EQ(pos, first + 4);
  *pos = 'x';
  EXPECT_EQ(str[4], 'x');
  EXPECT_EQ(FindChar(first, last, '\0'), last - 1);
}

TEST(FindChar, LargeRanges) {
  static constexpr std::size_t kSize = 4096;
  static constexpr std::size_t kPositions[]{0, 15, 16, 31, 63, 1000, kSize - 1};
  std::string str(kSize, 'x');
  const char* first = str.data();
  const char* last = first + str.size();
  EXPECT_EQ(FindChar(first, last, ' '), last);
  for (std::size_t pos : kPositions) {
    str[pos] = ' ';
    EXPECT_EQ(FindChar(first, last, ' '), first + pos) << pos;
    str[pos] = 'x';
  }
}

}  // namespace aeronet
