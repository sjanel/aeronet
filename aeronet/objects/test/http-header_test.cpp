#include "aeronet/http-header.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace aeronet {

TEST(HttpHeader, IsHeaderWhitespace) {
  EXPECT_TRUE(http::IsHeaderWhitespace(' '));
  EXPECT_TRUE(http::IsHeaderWhitespace('\t'));
  EXPECT_FALSE(http::IsHeaderWhitespace('A'));
  EXPECT_FALSE(http::IsHeaderWhitespace('\n'));
}

TEST(HttpHeader, IsValidHeaderName) {
  EXPECT_TRUE(http::IsValidHeaderName("Content-Type"));
  EXPECT_TRUE(http::IsValidHeaderName("X-Custom-Header_123"));
  EXPECT_FALSE(http::IsValidHeaderName("Invalid<Header"));  // invalid character
  EXPECT_FALSE(http::IsValidHeaderName(""));                // empty
  EXPECT_FALSE(http::IsValidHeaderName("Invalid Header"));  // space not allowed
  EXPECT_FALSE(http::IsValidHeaderName("Invalid:Header"));  // colon not allowed
  EXPECT_FALSE(http::IsValidHeaderName(""));                // empty name not allowed
}

TEST(HttpHeader, IsValidHeaderValue) {
  EXPECT_TRUE(http::IsValidHeaderValue("This is a valid header value."));
  EXPECT_TRUE(http::IsValidHeaderValue("Value with\ttab character."));
  EXPECT_FALSE(http::IsValidHeaderValue("Invalid value with \r carriage return."));
  EXPECT_FALSE(http::IsValidHeaderValue("Invalid value with \n line feed."));
  EXPECT_TRUE(http::IsValidHeaderValue(""));  // empty value is valid

  // Test some control characters (obs)
  EXPECT_FALSE(http::IsValidHeaderValue(std::string_view("\x01\x02\x03", 3)));  // control characters
  EXPECT_TRUE(http::IsValidHeaderValue(std::string_view("\x09\x20\x7E", 3)));   // HTAB and visible ASCII
}

TEST(HttpHeader, HeaderName) {
  EXPECT_EQ(http::Header("X-Test", "ValidValue ").name(), "X-Test");
  EXPECT_EQ(http::Header("Content-Length", " \t12345 ").name(), "Content-Length");
}

TEST(HttpHeader, HeaderValueTrimmed) {
  EXPECT_EQ(http::Header("X-Test", "  ValidValue  ").value(), "ValidValue");
  EXPECT_EQ(http::Header("Content-Length", "\t12345\t").value(), "12345");
  EXPECT_EQ(http::Header("Empty-Value", "   ").value(), "");  // trimmed to empty
}

TEST(HttpHeader, InvalidHeaderNameThrows) {
  EXPECT_THROW(http::Header("Invalid Header", "Value"), std::invalid_argument);  // space not allowed
  EXPECT_THROW(http::Header("Invalid<Header", "Value"), std::invalid_argument);  // invalid character
  EXPECT_THROW(http::Header("", "Value"), std::invalid_argument);                // empty name
}

TEST(HttpHeader, InvalidHeaderValueThrows) {
  EXPECT_THROW(http::Header("X-Test", "Invalid\rValue"), std::invalid_argument);  // CR not allowed
  EXPECT_THROW(http::Header("X-Test", "Invalid\nValue"), std::invalid_argument);  // LF not allowed
  EXPECT_NO_THROW(http::Header("X-Test", "Valid\tValue"));                        // HTAB allowed
  EXPECT_NO_THROW(http::Header("X-Test", ""));                                    // empty value allowed
}

TEST(HttpHeader, Raw) {
  http::Header header("X-Custom", "  Some Value  ");
  EXPECT_EQ(header.raw(), "X-Custom: Some Value");
}

TEST(HttpHeader, UnreasonableHeaderLen) {
  char ch{};
  std::string_view unreasonableHeaderLen(&ch, static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()));

  EXPECT_THROW(http::Header(unreasonableHeaderLen, "some value"), std::invalid_argument);
}

}  // namespace aeronet
