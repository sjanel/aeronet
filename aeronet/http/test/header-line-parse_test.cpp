#include "aeronet/header-line-parse.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace aeronet::http {
TEST(HeaderLineParseTest, ValidHeaderLine) {
  std::string_view line = "Content-Type: text/html";
  auto [name, value] = ParseHeaderLine(line.data(), line.data() + line.size());
  EXPECT_EQ(name, "Content-Type");
  EXPECT_EQ(value, "text/html");
}

TEST(HeaderLineParseTest, HeaderLineWithLeadingAndTrailingWhitespace) {
  std::string_view line = "   X-Custom-Header:    some value   ";
  auto [name, value] = ParseHeaderLine(line.data(), line.data() + line.size());
  EXPECT_EQ(name, "   X-Custom-Header");
  EXPECT_EQ(value, "some value");
}

TEST(HeaderLineParseTest, HeaderLineWithNoValue) {
  std::string_view line = "X-Empty-Header:   ";
  auto [name, value] = ParseHeaderLine(line.data(), line.data() + line.size());
  EXPECT_EQ(name, "X-Empty-Header");
  EXPECT_EQ(value, "");
}

TEST(HeaderLineParseTest, HeaderLineWithNoWhitespace) {
  std::string_view line = "X-NoSpace:Value";
  auto [name, value] = ParseHeaderLine(line.data(), line.data() + line.size());
  EXPECT_EQ(name, "X-NoSpace");
  EXPECT_EQ(value, "Value");
}

TEST(HeaderLineParseTest, HeaderLineWithOnlyColon) {
  std::string_view line = ":";
  auto [name, value] = ParseHeaderLine(line.data(), line.data() + line.size());
  EXPECT_EQ(name, "");
  EXPECT_EQ(value, "");
}

TEST(HeaderLineParseTest, HeaderLineMissingColon) {
  std::string_view line = "InvalidHeaderLineWithoutColon";
  auto [name, value] = ParseHeaderLine(line.data(), line.data() + line.size());
  EXPECT_TRUE(name.empty());
}

}  // namespace aeronet::http
