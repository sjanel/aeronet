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

  // Every byte is checked: exactly the tchar are allowed (RFC 9110 §5.6.2).
  for (unsigned int byte = 0; byte < 256U; ++byte) {
    const char ch = static_cast<char>(byte);
    const bool expected = (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
                          (byte >= 'a' && byte <= 'z') || std::string_view("!#$%&'*+-.^_`|~").contains(ch);
    EXPECT_EQ(IsValidHeaderName(std::string_view(&ch, 1)), expected) << byte;
    const char name[] = {'x', ch, 'y'};
    EXPECT_EQ(IsValidHeaderName(std::string_view(name, sizeof(name))), expected) << byte;
  }
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

  // obs-text is not sent
  EXPECT_FALSE(IsValidHeaderValue("caf\xC3\xA9"));
  EXPECT_FALSE(IsValidHeaderValue("\x80"));
  EXPECT_FALSE(IsValidHeaderValue("\xFF"));
}

TEST(HttpHeader, IsValidReceivedHeaderValue) {
  EXPECT_TRUE(IsValidReceivedHeaderValue(""));
  EXPECT_TRUE(IsValidReceivedHeaderValue("This is a valid header value."));
  EXPECT_TRUE(IsValidReceivedHeaderValue("Value with\ttab character."));
  EXPECT_TRUE(IsValidReceivedHeaderValue("attachment; filename=\"caf\xC3\xA9.txt\""));  // UTF-8
  EXPECT_TRUE(IsValidReceivedHeaderValue("\xE9t\xE9"));                                 // ISO-8859-1

  // Every byte is checked: exactly HTAB, SP, VCHAR and obs-text are allowed (RFC 9110 §5.5).
  for (unsigned int byte = 0; byte < 256U; ++byte) {
    const char ch = static_cast<char>(byte);
    const bool expected = byte == '\t' || (byte >= 0x20U && byte != 0x7FU);
    EXPECT_EQ(IsValidReceivedHeaderValue(std::string_view(&ch, 1)), expected) << byte;
    EXPECT_EQ(IsValidHeaderValue(std::string_view(&ch, 1)), expected && byte < 0x80U) << byte;
  }

  EXPECT_FALSE(IsValidReceivedHeaderValue("caf\xC3\xA9\r"));
  EXPECT_FALSE(IsValidReceivedHeaderValue("caf\xC3\xA9\n"));
  EXPECT_FALSE(IsValidReceivedHeaderValue(std::string_view("caf\xC3\xA9\0", 6)));
  EXPECT_FALSE(IsValidReceivedHeaderValue("caf\xC3\xA9\x7F"));
}

static_assert(IsValidHeaderName("content-type"));
static_assert(IsValidReceivedHeaderValue("caf\xC3\xA9"));
static_assert(!IsValidHeaderValue("caf\xC3\xA9"));

}  // namespace aeronet::http
