#include "aeronet/http-header.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

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
  EXPECT_EQ(header.http1Raw(), "X-Custom: Some Value");
}

TEST(HttpHeader, UnreasonableHeaderLen) {
  char ch{};
  std::string_view unreasonableHeaderLen(&ch, static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()));

  EXPECT_THROW(http::Header(unreasonableHeaderLen, "some value"), std::invalid_argument);
}

TEST(HttpHeader, MoveConstructor) {
  http::Header original("X-Move-Test", "MoveValue");
  http::Header moved(std::move(original));

  EXPECT_EQ(moved.name(), "X-Move-Test");
  EXPECT_EQ(moved.value(), "MoveValue");
  EXPECT_EQ(moved.http1Raw(), "X-Move-Test: MoveValue");
}

TEST(HttpHeader, MoveAssignment) {
  http::Header original("X-Move-Assign", "MoveAssignValue");
  http::Header toBeMoved("Temp-Header", "TempValue");
  toBeMoved = std::move(original);

  EXPECT_EQ(toBeMoved.name(), "X-Move-Assign");
  EXPECT_EQ(toBeMoved.value(), "MoveAssignValue");
  EXPECT_EQ(toBeMoved.http1Raw(), "X-Move-Assign: MoveAssignValue");
}

TEST(HttpHeader, CopyConstructor) {
  http::Header original("X-Copy-Test", "CopyValue");
  http::Header copy(original);  // NOLINT(performance-unnecessary-copy-initialization)

  EXPECT_EQ(copy.name(), original.name());
  EXPECT_EQ(copy.value(), original.value());
  EXPECT_EQ(copy.http1Raw(), original.http1Raw());
}

TEST(HttpHeader, CopyAssignment) {
  http::Header original("X-Assign-Test", "AssignValue");
  http::Header assigned("Temp-Header", "TempValue");
  assigned = original;
  EXPECT_EQ(assigned.name(), original.name());
  EXPECT_EQ(assigned.value(), original.value());
  EXPECT_EQ(assigned.http1Raw(), original.http1Raw());

  original = assigned;
  EXPECT_EQ(original.name(), assigned.name());
  EXPECT_EQ(original.value(), assigned.value());
}

TEST(HttpHeader, SelfCopyAssignment) {
  http::Header original("X-Self-Assign", "SelfValue");
  auto &alias = original;
  original = alias;  // Self-assignment
  EXPECT_EQ(original.name(), "X-Self-Assign");
  EXPECT_EQ(original.value(), "SelfValue");
}

}  // namespace aeronet
