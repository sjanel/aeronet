#include "aeronet/http-header-is-valid.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace aeronet::http {

TEST(HttpHeader, IsValidHeaderName) {
  EXPECT_TRUE(IsValidHeaderName("Content-Type"));
  EXPECT_TRUE(IsValidHeaderName("X-Custom-Header_123"));
  EXPECT_FALSE(IsValidHeaderName("Invalid<Header"));  // invalid character
  EXPECT_FALSE(IsValidHeaderName(""));                // empty
  EXPECT_FALSE(IsValidHeaderName("Invalid Header"));  // space not allowed
  EXPECT_FALSE(IsValidHeaderName("Invalid:Header"));  // colon not allowed
  EXPECT_FALSE(IsValidHeaderName(""));                // empty name not allowed
}

TEST(HttpHeader, IsValidHeaderValue) {
  EXPECT_TRUE(IsValidHeaderValue("This is a valid header value."));
  EXPECT_TRUE(IsValidHeaderValue("Value with\ttab character."));
  EXPECT_FALSE(IsValidHeaderValue("Invalid value with \r carriage return."));
  EXPECT_FALSE(IsValidHeaderValue("Invalid value with \n line feed."));
  EXPECT_TRUE(IsValidHeaderValue(""));  // empty value is valid

  // Test some control characters (obs)
  EXPECT_FALSE(IsValidHeaderValue(std::string_view("\x01\x02\x03", 3)));  // control characters
  EXPECT_TRUE(IsValidHeaderValue(std::string_view("\x09\x20\x7E", 3)));   // HTAB and visible ASCII
}

}  // namespace aeronet::http
