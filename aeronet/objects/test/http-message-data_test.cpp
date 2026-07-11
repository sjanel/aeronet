#include "aeronet/http-message-data.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/http-payload.hpp"
#include "aeronet/raw-chars.hpp"

using namespace aeronet;

// Test default constructor
TEST(HttpResponseDataTest, DefaultConstructor) {
  HttpMessageData data;

  EXPECT_TRUE(data.empty());
  EXPECT_EQ(data.remainingSize(), 0U);
  EXPECT_EQ(data.firstBuffer(), "");
  EXPECT_EQ(data.secondBuffer(), "");
}

// Test string_view constructor
TEST(HttpResponseDataTest, StringViewConstructor) {
  const std::string_view content = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nHello";
  HttpMessageData data(content);

  EXPECT_FALSE(data.empty());
  EXPECT_EQ(data.remainingSize(), content.size());
  EXPECT_EQ(data.firstBuffer(), content);
  EXPECT_EQ(data.secondBuffer(), "");
}

// Test RawChars constructor
TEST(HttpResponseDataTest, RawCharsConstructor) {
  RawChars head("HTTP/1.1 200 OK\r\n\r\n");
  const std::size_t headSize = head.size();
  HttpMessageData data(std::move(head));

  EXPECT_FALSE(data.empty());
  EXPECT_EQ(data.remainingSize(), headSize);
  EXPECT_EQ(data.firstBuffer(), "HTTP/1.1 200 OK\r\n\r\n");
  EXPECT_EQ(data.secondBuffer(), "");
}

// Test constructor with RawChars head and HttpPayload body
TEST(HttpResponseDataTest, HeadAndBodyConstructorWithRawChars) {
  RawChars head("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n");
  HttpPayload body(std::string("Hello"));
  const std::size_t totalSize = head.size() + body.size();

  HttpMessageData data(std::move(head), std::move(body));

  EXPECT_FALSE(data.empty());
  EXPECT_EQ(data.remainingSize(), totalSize);
  EXPECT_EQ(data.firstBuffer(), "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n");
  EXPECT_EQ(data.secondBuffer(), "Hello");
}

// Test constructor with string_view head and HttpPayload body
TEST(HttpResponseDataTest, HeadAndBodyConstructorWithStringView) {
  const std::string_view head = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
  HttpPayload body(std::string("World"));
  const std::size_t totalSize = head.size() + body.size();

  HttpMessageData data(head, std::move(body));

  EXPECT_FALSE(data.empty());
  EXPECT_EQ(data.remainingSize(), totalSize);
  EXPECT_EQ(data.firstBuffer(), head);
  EXPECT_EQ(data.secondBuffer(), "World");
}

// Test firstBuffer method
TEST(HttpResponseDataTest, FirstBuffer) {
  HttpMessageData data("Test data");

  EXPECT_EQ(data.firstBuffer(), "Test data");

  // After adding offset
  data.addOffset(5);
  EXPECT_EQ(data.firstBuffer(), "data");

  // After offset reaches end
  data.addOffset(4);
  EXPECT_EQ(data.firstBuffer(), "");
}

// Test secondBuffer method
TEST(HttpResponseDataTest, SecondBuffer) {
  RawChars head("Header");
  HttpPayload body(std::string("BodyContent"));
  HttpMessageData data(std::move(head), std::move(body));

  EXPECT_EQ(data.secondBuffer(), "BodyContent");

  // After offset in first buffer
  data.addOffset(3);
  EXPECT_EQ(data.secondBuffer(), "BodyContent");

  // After offset past first buffer
  data.addOffset(3);
  EXPECT_EQ(data.secondBuffer(), "BodyContent");

  // After offset into second buffer
  data.addOffset(4);
  EXPECT_EQ(data.secondBuffer(), "Content");
}

// Test remainingSize method
TEST(HttpResponseDataTest, RemainingSize) {
  RawChars head("Header");
  HttpPayload body(std::string("Body"));
  HttpMessageData data(std::move(head), std::move(body));

  EXPECT_EQ(data.remainingSize(), 10U);  // 6 + 4

  data.addOffset(3);
  EXPECT_EQ(data.remainingSize(), 7U);

  data.addOffset(7);
  EXPECT_EQ(data.remainingSize(), 0U);
}

// Test empty method
TEST(HttpResponseDataTest, Empty) {
  HttpMessageData data;
  EXPECT_TRUE(data.empty());

  data.append("Test");
  EXPECT_FALSE(data.empty());

  data.addOffset(4);
  EXPECT_TRUE(data.empty());
}

// Test addOffset method
TEST(HttpResponseDataTest, AddOffset) {
  HttpMessageData data("0123456789");

  data.addOffset(5);
  EXPECT_EQ(data.firstBuffer(), "56789");
  EXPECT_EQ(data.remainingSize(), 5U);

  data.addOffset(5);
  EXPECT_EQ(data.firstBuffer(), "");
  EXPECT_EQ(data.remainingSize(), 0U);
}

// Test addOffset with both head and body
TEST(HttpResponseDataTest, AddOffsetWithHeadAndBody) {
  RawChars head("ABCDE");
  HttpPayload body(std::string("12345"));
  HttpMessageData data(std::move(head), std::move(body));

  EXPECT_EQ(data.remainingSize(), 10U);

  data.addOffset(2);
  EXPECT_EQ(data.firstBuffer(), "CDE");
  EXPECT_EQ(data.secondBuffer(), "12345");
  EXPECT_EQ(data.remainingSize(), 8U);

  data.addOffset(3);
  EXPECT_EQ(data.firstBuffer(), "");
  EXPECT_EQ(data.secondBuffer(), "12345");
  EXPECT_EQ(data.remainingSize(), 5U);

  data.addOffset(2);
  EXPECT_EQ(data.firstBuffer(), "");
  EXPECT_EQ(data.secondBuffer(), "345");
  EXPECT_EQ(data.remainingSize(), 3U);

  data.addOffset(3);
  EXPECT_EQ(data.remainingSize(), 0U);
  EXPECT_TRUE(data.empty());
}

// Test append method with HttpMessageData (no body set initially)
TEST(HttpResponseDataTest, AppendHttpResponseDataNoBody) {
  HttpMessageData data1("First");
  HttpMessageData data2("Second");

  data1.append(std::move(data2));

  EXPECT_EQ(data1.firstBuffer(), "FirstSecond");
  EXPECT_EQ(data1.remainingSize(), 11U);
}

// Test append method with HttpMessageData (body already set)
TEST(HttpResponseDataTest, AppendHttpResponseDataWithBody) {
  RawChars head("Header");
  HttpPayload body(std::string("Body"));
  HttpMessageData data1(std::move(head), std::move(body));

  HttpMessageData data2("Extra");
  data1.append(std::move(data2));

  EXPECT_EQ(data1.firstBuffer(), "Header");
  EXPECT_EQ(data1.secondBuffer(), "BodyExtra");
  EXPECT_EQ(data1.remainingSize(), 15U);
}

// Test append method with HttpMessageData containing both head and body
TEST(HttpResponseDataTest, AppendHttpResponseDataBothWithBody) {
  RawChars head1("Head1");
  HttpPayload body1(std::string("Body1"));
  HttpMessageData data1(std::move(head1), std::move(body1));

  RawChars head2("Head2");
  HttpPayload body2(std::string("Body2"));
  HttpMessageData data2(std::move(head2), std::move(body2));

  data1.append(std::move(data2));

  EXPECT_EQ(data1.firstBuffer(), "Head1");
  EXPECT_EQ(data1.secondBuffer(), "Body1Head2Body2");
  EXPECT_EQ(data1.remainingSize(), 20U);
}

// Test append method with string_view (no body set)
TEST(HttpResponseDataTest, AppendStringViewNoBody) {
  HttpMessageData data("Initial");

  data.append(" content");

  EXPECT_EQ(data.firstBuffer(), "Initial content");
  EXPECT_EQ(data.remainingSize(), 15U);
}

// Test append method with string_view (body already set)
TEST(HttpResponseDataTest, AppendStringViewWithBody) {
  RawChars head("Header");
  HttpPayload body(std::string("Body"));
  HttpMessageData data(std::move(head), std::move(body));

  data.append(" extra");

  EXPECT_EQ(data.firstBuffer(), "Header");
  EXPECT_EQ(data.secondBuffer(), "Body extra");
  EXPECT_EQ(data.remainingSize(), 16U);
}

// Test append method with const char* + size (body already set)
TEST(HttpResponseDataTest, AppendCharPointerWithBody) {
  RawChars head("Header");
  HttpPayload body(std::string("Body"));
  HttpMessageData data(std::move(head), std::move(body));

  const char extra[] = " plus";
  data.append(extra, 5U);

  EXPECT_EQ(data.firstBuffer(), "Header");
  EXPECT_EQ(data.secondBuffer(), "Body plus");
  EXPECT_EQ(data.remainingSize(), 15U);
}

// Test multiple appends
TEST(HttpResponseDataTest, MultipleAppends) {
  HttpMessageData data;

  data.append("First");
  data.append(" Second");
  data.append(" Third");

  EXPECT_EQ(data.firstBuffer(), "First Second Third");
  EXPECT_EQ(data.remainingSize(), 18U);
}

// Test clear method
TEST(HttpResponseDataTest, Clear) {
  RawChars head("Header");
  HttpPayload body(std::string("Body"));
  HttpMessageData data(std::move(head), std::move(body));

  data.addOffset(3);
  EXPECT_FALSE(data.empty());

  data.clear();

  EXPECT_TRUE(data.empty());
  EXPECT_EQ(data.remainingSize(), 0U);
  EXPECT_EQ(data.firstBuffer(), "");
  EXPECT_EQ(data.secondBuffer(), "");
}

// Test shrink_to_fit method
TEST(HttpResponseDataTest, ShrinkToFit) {
  RawChars head(1024);
  head.append("Small");
  HttpPayload body(std::string("Content"));
  HttpMessageData data(std::move(head), std::move(body));

  data.shrink_to_fit();

  EXPECT_EQ(data.firstBuffer(), "Small");
  EXPECT_EQ(data.secondBuffer(), "Content");
  EXPECT_EQ(data.remainingSize(), 12U);
}

// Test with empty strings
TEST(HttpResponseDataTest, EmptyStrings) {
  HttpMessageData data1("");
  EXPECT_TRUE(data1.empty());

  HttpMessageData data2;
  data2.append("");
  EXPECT_TRUE(data2.empty());

  RawChars head("");
  HttpPayload body(std::string(""));
  HttpMessageData data3(std::move(head), std::move(body));
  EXPECT_TRUE(data3.empty());
}

// Test large data
TEST(HttpResponseDataTest, LargeData) {
  const std::string largeHead(10000, 'H');
  const std::string largeBody(20000, 'B');

  RawChars head(largeHead);
  HttpPayload body{std::string(largeBody)};
  HttpMessageData data(std::move(head), std::move(body));

  EXPECT_EQ(data.remainingSize(), 30000U);
  EXPECT_EQ(data.firstBuffer().size(), 10000U);
  EXPECT_EQ(data.secondBuffer().size(), 20000U);

  data.addOffset(10000);
  EXPECT_EQ(data.firstBuffer(), "");
  EXPECT_EQ(data.secondBuffer().size(), 20000U);
  EXPECT_EQ(data.remainingSize(), 20000U);
}

// Test with various HttpPayload types (vector<char>)
TEST(HttpResponseDataTest, PayloadWithVectorChar) {
  RawChars head("Header");
  std::vector<char> bodyVec = {'B', 'o', 'd', 'y'};
  HttpPayload body(std::move(bodyVec));
  HttpMessageData data(std::move(head), std::move(body));

  EXPECT_EQ(data.secondBuffer(), "Body");
  EXPECT_EQ(data.remainingSize(), 10U);
}

// Test with various HttpPayload types (vector<byte>)
TEST(HttpResponseDataTest, PayloadWithVectorByte) {
  RawChars head("Header");
  std::vector<std::byte> bodyVec = {std::byte{'D'}, std::byte{'a'}, std::byte{'t'}, std::byte{'a'}};
  HttpPayload body(std::move(bodyVec));
  HttpMessageData data(std::move(head), std::move(body));

  EXPECT_EQ(data.secondBuffer(), "Data");
  EXPECT_EQ(data.remainingSize(), 10U);
}

// Test with various HttpPayload types (unique_ptr<char[]>)
TEST(HttpResponseDataTest, PayloadWithUniquePtr) {
  RawChars head("Header");
  auto buf = std::make_unique<char[]>(4);
  std::memcpy(buf.get(), "Test", 4);
  HttpPayload body(std::move(buf), 4);
  HttpMessageData data(std::move(head), std::move(body));

  EXPECT_EQ(data.secondBuffer(), "Test");
  EXPECT_EQ(data.remainingSize(), 10U);
}

// Test offset boundary conditions
TEST(HttpResponseDataTest, OffsetBoundaries) {
  RawChars head("12345");
  HttpPayload body(std::string("67890"));
  HttpMessageData data(std::move(head), std::move(body));

  // Offset at boundary between head and body
  data.addOffset(5);
  EXPECT_EQ(data.firstBuffer(), "");
  EXPECT_EQ(data.secondBuffer(), "67890");
  EXPECT_EQ(data.remainingSize(), 5U);

  // Continue into body
  data.addOffset(1);
  EXPECT_EQ(data.secondBuffer(), "7890");
  EXPECT_EQ(data.remainingSize(), 4U);
}

// Test append after offset
TEST(HttpResponseDataTest, AppendAfterOffset) {
  HttpMessageData data("Initial");
  data.addOffset(3);

  data.append(" More");

  // After offset, firstBuffer shows remaining data; append adds to the end
  EXPECT_EQ(data.firstBuffer(), "tial More");
  EXPECT_EQ(data.remainingSize(), 9U);  // "itial" (4) + " More" (5) = 9
}

// Test sequential operations
TEST(HttpResponseDataTest, SequentialOperations) {
  HttpMessageData data;

  // Build up data
  data.append("HTTP/1.1 200 OK\r\n");
  data.append("Content-Length: 5\r\n\r\n");

  EXPECT_EQ(data.remainingSize(), 38U);

  // Consume some
  data.addOffset(17);
  EXPECT_EQ(data.firstBuffer(), "Content-Length: 5\r\n\r\n");

  // Add more
  data.append("Body");
  EXPECT_EQ(data.remainingSize(), 25U);

  // Consume rest
  data.addOffset(25);
  EXPECT_TRUE(data.empty());

  // Clear and reuse
  data.clear();
  data.append("New content");
  EXPECT_EQ(data.remainingSize(), 11U);
}

// Test move semantics
TEST(HttpResponseDataTest, MoveSemantics) {
  RawChars head("Header");
  HttpPayload body(std::string("Body"));
  HttpMessageData data1(std::move(head), std::move(body));

  HttpMessageData data2 = std::move(data1);

  EXPECT_EQ(data2.firstBuffer(), "Header");
  EXPECT_EQ(data2.secondBuffer(), "Body");
  EXPECT_EQ(data2.remainingSize(), 10U);
}

// Test append empty HttpMessageData
TEST(HttpResponseDataTest, AppendEmptyHttpResponseData) {
  HttpMessageData data1("Content");
  HttpMessageData data2;

  data1.append(std::move(data2));

  EXPECT_EQ(data1.firstBuffer(), "Content");
  EXPECT_EQ(data1.remainingSize(), 7U);
}

// Test mixing append operations
TEST(HttpResponseDataTest, MixedAppendOperations) {
  HttpMessageData data("Start");

  HttpMessageData other1(" Middle");
  data.append(std::move(other1));

  data.append(" End");

  EXPECT_EQ(data.firstBuffer(), "Start Middle End");
  EXPECT_EQ(data.remainingSize(), 16U);
}

// Test RawChars with reserved capacity
TEST(HttpResponseDataTest, RawCharsWithReservedCapacity) {
  RawChars head(1000);
  head.append("Small content");
  HttpMessageData data(std::move(head));

  EXPECT_EQ(data.firstBuffer(), "Small content");
  EXPECT_EQ(data.remainingSize(), 13U);
}

// Test body transition during append
TEST(HttpResponseDataTest, BodyTransitionDuringAppend) {
  HttpMessageData data("Initial");

  // First append creates head without body
  data.append(" text");
  EXPECT_EQ(data.firstBuffer(), "Initial text");
  EXPECT_EQ(data.secondBuffer(), "");

  // Now add body via HttpMessageData append
  RawChars head2("Head2");
  HttpPayload body2(std::string("Body2"));
  HttpMessageData data2(std::move(head2), std::move(body2));

  data.append(std::move(data2));

  // Since capturedBody was not set initially, everything goes into headAndOptionalBody
  EXPECT_EQ(data.firstBuffer(), "Initial textHead2");
  EXPECT_EQ(data.secondBuffer(), "Body2");
}

// Test offset beyond total size (edge case - causes underflow with size_t)
TEST(HttpResponseDataTest, OffsetBeyondSize) {
  HttpMessageData data("Short");

  data.addOffset(10);  // Offset beyond content

  EXPECT_EQ(data.firstBuffer(), "");
  // Note: This causes size_t underflow since offset > size
  // remainingSize() = size - offset where size=5, offset=10 -> wraps around
  // This is expected behavior - caller should not offset beyond size
}

// Test clear after partial consumption
TEST(HttpResponseDataTest, ClearAfterPartialConsumption) {
  HttpMessageData data("Content to consume");
  data.addOffset(7);

  EXPECT_EQ(data.firstBuffer(), " to consume");

  data.clear();

  EXPECT_TRUE(data.empty());
  EXPECT_EQ(data.firstBuffer(), "");
}

// Test append after clear
TEST(HttpResponseDataTest, AppendAfterClear) {
  HttpMessageData data("Old content");
  data.clear();

  data.append("New content");

  EXPECT_EQ(data.firstBuffer(), "New content");
  EXPECT_EQ(data.remainingSize(), 11U);
}

// Test with binary data
TEST(HttpResponseDataTest, BinaryData) {
  const std::string binaryData = std::string("\x00\x01\x02\x03\x04", 5);
  HttpMessageData data(binaryData);

  EXPECT_EQ(data.remainingSize(), 5U);
  EXPECT_EQ(data.firstBuffer().size(), 5U);
  EXPECT_EQ(std::memcmp(data.firstBuffer().data(), binaryData.data(), 5), 0);
}
