#include "aeronet/http-payload.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "raw-chars.hpp"

using namespace aeronet;

TEST(HttpBody, DefaultConstructedIsUnset) {
  HttpPayload body;
  EXPECT_FALSE(body.set());
  EXPECT_EQ(body.size(), 0U);
  EXPECT_EQ(body.view().size(), 0U);
}

TEST(HttpBody, ConstructFromString) {
  HttpPayload body(std::string("hello"));
  EXPECT_TRUE(body.set());
  EXPECT_EQ(body.size(), 5U);
  EXPECT_EQ(body.view(), "hello");
}

TEST(HttpBody, ConstructFromVectorChar) {
  std::vector<char> vec{'a', 'b', 'c'};
  HttpPayload body(std::move(vec));
  EXPECT_TRUE(body.set());
  EXPECT_EQ(body.size(), 3U);
  EXPECT_EQ(body.view(), "abc");
}

TEST(HttpBody, ConstructFromUniqueBuffer) {
  auto buf = std::make_unique<char[]>(4);
  std::memcpy(buf.get(), "abcd", 4);
  HttpPayload body(std::move(buf), 4);
  EXPECT_TRUE(body.set());
  EXPECT_EQ(body.size(), 4U);
  EXPECT_EQ(body.view(), std::string_view("abcd", 4));
}

TEST(HttpBody, ConstructFromRawChars) {
  RawChars rawChars(std::string_view("xyz", 3));
  HttpPayload body(std::move(rawChars));
  EXPECT_TRUE(body.set());
  EXPECT_EQ(body.size(), 3U);
  EXPECT_EQ(body.view(), "xyz");
}

TEST(HttpBody, AppendStringToString) {
  HttpPayload body(std::string("foo"));
  body.append(std::string_view("bar"));
  EXPECT_EQ(body.view(), "foobar");
}

TEST(HttpBody, AppendStringViewToVector) {
  std::vector<char> vec{'1', '2'};
  HttpPayload body(std::move(vec));
  body.append(std::string_view("34"));
  EXPECT_EQ(body.view(), "1234");
}

TEST(HttpBody, AppendHttpBodyToString) {
  HttpPayload body1(std::string("head"));
  HttpPayload body2(std::string("tail"));
  body1.append(body2);
  EXPECT_EQ(body1.view(), "headtail");
}

TEST(HttpBody, AppendHttpBodyToMonostateAdopts) {
  HttpPayload body1;  // monostate
  HttpPayload body2(std::string("adopted"));
  body1.append(body2);
  EXPECT_TRUE(body1.set());
  EXPECT_EQ(body1.view(), "adopted");
}

TEST(HttpBody, AppendLargeToCharBuffer) {
  auto buf = std::make_unique<char[]>(3);
  std::memcpy(buf.get(), "ABC", 3);
  HttpPayload body(std::move(buf), 3);
  body.append(std::string_view("DEF"));
  EXPECT_EQ(body.size(), 6U);
  EXPECT_EQ(body.view(), "ABCDEF");
}

TEST(HttpBody, ClearResetsSizeOrZeroesBuffer) {
  HttpPayload body1(std::string("toreset"));
  EXPECT_EQ(body1.size(), 7U);
  body1.clear();
  EXPECT_EQ(body1.size(), 0U);
  HttpPayload body2(std::vector<char>{'x', 'y'});
  body2.clear();
  EXPECT_EQ(body2.size(), 0U);
  auto buf = std::make_unique<char[]>(5);
  std::memcpy(buf.get(), "hello", 5);
  HttpPayload body3(std::move(buf), 5);
  body3.clear();
  EXPECT_EQ(body3.size(), 0U);
}

TEST(HttpBody, MultipleAppendCombinations) {
  HttpPayload dst(std::string("A"));
  HttpPayload src(std::vector<char>{'B', 'C'});
  dst.append(src);
  dst.append(std::string_view("D"));
  EXPECT_EQ(dst.view(), "ABCD");

  HttpPayload dst2;  // start empty
  dst2.append(dst);
  EXPECT_EQ(dst2.view(), "ABCD");
}

// Ensure that view remains valid and consistent across operations
TEST(HttpBody, ViewStabilityAfterAppend) {
  HttpPayload body1(std::string("start"));
  std::string_view data = body1.view();
  EXPECT_EQ(data, "start");
  body1.append(std::string_view("-more"));
  EXPECT_EQ(body1.view(), "start-more");
}
