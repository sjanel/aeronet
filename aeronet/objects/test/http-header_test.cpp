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
  auto& alias = original;
  original = alias;  // Self-assignment
  EXPECT_EQ(original.name(), "X-Self-Assign");
  EXPECT_EQ(original.value(), "SelfValue");
}

TEST(HttpHeader, ReverseIteratorEmpty) {
  std::string_view headerValue;
  http::HeaderValueReverseTokensIterator<','> it(headerValue);

  EXPECT_FALSE(it.hasNext());
}

TEST(HttpHeader, ReverseIteratorSingleValue) {
  std::string_view headerValue = "singleValue";
  http::HeaderValueReverseTokensIterator<','> it(headerValue);

  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "singleValue");

  EXPECT_FALSE(it.hasNext());
}

TEST(HttpHeader, ReverseIteratorTwoValuesNoSpaces) {
  std::string_view headerValue = "gzip,deflate";
  http::HeaderValueReverseTokensIterator<','> it(headerValue);

  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "deflate");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "gzip");

  EXPECT_FALSE(it.hasNext());
}

TEST(HttpHeader, ReverseIteratorMultipleWithSpacesAndEmpty) {
  std::string_view headerValue = "gzip, deflate , br ,  ,  identity";
  http::HeaderValueReverseTokensIterator<','> it(headerValue);

  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "identity");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "br");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "deflate");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "gzip");

  EXPECT_FALSE(it.hasNext());
}

TEST(HttpHeader, ReverseIteratorEmptyOnSides1) {
  std::string_view headerValue = ", value1 ,\tvalue2 ,";
  http::HeaderValueReverseTokensIterator<','> it(headerValue);

  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "value2");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "value1");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "");

  EXPECT_FALSE(it.hasNext());
}

TEST(HttpHeader, ReverseIteratorEmptyOnSides2) {
  std::string_view headerValue = ", ,, , value2 ,";
  http::HeaderValueReverseTokensIterator<','> it(headerValue);

  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "value2");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "");

  EXPECT_FALSE(it.hasNext());
}

TEST(HttpHeader, ReverseIteratorWithQuotedValues) {
  std::string_view headerValue = R"(gzip, ," ","quoted, value")";
  http::HeaderValueReverseTokensIterator<','> it(headerValue);

  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), R"("quoted, value")");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), R"(" ")");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), "gzip");

  EXPECT_FALSE(it.hasNext());
}

TEST(HttpHeader, ReverseIteratorWithEscapedQuotes) {
  std::string_view headerValue = R"("\"gzip", "value with \"escaped\" quotes", "deflate\"")";
  http::HeaderValueReverseTokensIterator<','> it(headerValue);

  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), R"("deflate\"")");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), R"("value with \"escaped\" quotes")");
  ASSERT_TRUE(it.hasNext());
  EXPECT_EQ(it.next(), R"("\"gzip")");

  EXPECT_FALSE(it.hasNext());
}

}  // namespace aeronet
