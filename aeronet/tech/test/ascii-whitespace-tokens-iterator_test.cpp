#include "aeronet/ascii-whitespace-tokens-iterator.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace aeronet {

namespace {

std::vector<std::string_view> Tokens(std::string_view str) {
  std::vector<std::string_view> tokens;
  for (AsciiWhitespaceTokensIterator it(str); it.hasNext();) {
    tokens.push_back(it.next());
  }
  return tokens;
}

constexpr bool CountsTwoTokens() {
  AsciiWhitespaceTokensIterator it(" a\tbc ");
  return it.hasNext() && it.next() == "a" && it.hasNext() && it.next() == "bc" && !it.hasNext();
}

static_assert(CountsTwoTokens());

}  // namespace

TEST(AsciiWhitespaceTokensIterator, NoToken) {
  EXPECT_TRUE(Tokens({}).empty());
  EXPECT_TRUE(Tokens("").empty());
  EXPECT_TRUE(Tokens(" \t\n\f\r").empty());
}

TEST(AsciiWhitespaceTokensIterator, SingleToken) {
  EXPECT_EQ(Tokens("a"), std::vector<std::string_view>{"a"});
  EXPECT_EQ(Tokens("  token  "), std::vector<std::string_view>{"token"});
}

TEST(AsciiWhitespaceTokensIterator, SeparatedByAnyAsciiWhitespace) {
  EXPECT_EQ(Tokens("a b\tc\nd\fe\rf"), (std::vector<std::string_view>{"a", "b", "c", "d", "e", "f"}));
  EXPECT_EQ(Tokens("\r\n first \t\t second\f"), (std::vector<std::string_view>{"first", "second"}));
}

TEST(AsciiWhitespaceTokensIterator, VerticalTabAndOtherCharactersAreNotWhitespace) {
  EXPECT_EQ(Tokens("a\vb c"), (std::vector<std::string_view>{"a\vb", "c"}));
  const std::string withNul("x\0y z", 5);
  EXPECT_EQ(Tokens(withNul), (std::vector<std::string_view>{std::string_view("x\0y", 3), "z"}));
}

TEST(AsciiWhitespaceTokensIterator, TokensViewTheInput) {
  const std::string_view input = " ab cd";
  AsciiWhitespaceTokensIterator it(input);
  ASSERT_TRUE(it.hasNext());
  const std::string_view first = it.next();
  EXPECT_EQ(first.data(), input.data() + 1);
  ASSERT_TRUE(it.hasNext());
  const std::string_view second = it.next();
  EXPECT_EQ(second.data(), input.data() + 4);
  EXPECT_EQ(second.size(), 2U);
  EXPECT_FALSE(it.hasNext());
}

TEST(AsciiWhitespaceTokensIterator, LargeInput) {
  std::string input;
  constexpr int kNbTokens = 10000;
  for (int index = 0; index < kNbTokens; ++index) {
    input.append(static_cast<std::size_t>(index % 3) + 1, ' ').append("token").append(std::to_string(index));
  }
  const std::vector<std::string_view> tokens = Tokens(input);
  ASSERT_EQ(tokens.size(), static_cast<std::size_t>(kNbTokens));
  EXPECT_EQ(tokens.front(), "token0");
  EXPECT_EQ(tokens.back(), "token9999");
}

}  // namespace aeronet
