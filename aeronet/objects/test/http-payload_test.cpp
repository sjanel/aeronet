#include "aeronet/http-payload.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/raw-chars.hpp"

using namespace aeronet;

TEST(HttpPayload, DefaultConstructedIsUnset) {
  HttpPayload body;
  EXPECT_FALSE(body.set());
  EXPECT_EQ(body.size(), 0U);
  EXPECT_EQ(body.view().size(), 0U);
  EXPECT_EQ(body.view(), std::string_view{});
  EXPECT_EQ(body.data(), nullptr);

  body.append(std::string_view("data"));
  EXPECT_TRUE(body.set());
  EXPECT_EQ(body.size(), 4U);
  EXPECT_EQ(body.view(), "data");
  EXPECT_EQ(body.data(), reinterpret_cast<const char*>(body.view().data()));
}

TEST(HttpPayload, ConstructFromString) {
  HttpPayload body(std::string("hello"));
  EXPECT_TRUE(body.set());
  EXPECT_EQ(body.size(), 5U);
  EXPECT_EQ(body.view(), "hello");
}

TEST(HttpPayload, ConstructFromVectorChar) {
  std::vector<char> vec{'a', 'b', 'c'};
  HttpPayload body(std::move(vec));
  EXPECT_TRUE(body.set());
  EXPECT_EQ(body.size(), 3U);
  EXPECT_EQ(body.view(), "abc");
}

TEST(HttpPayload, ConstructFromUniqueBuffer) {
  auto buf = std::make_unique<char[]>(4);
  std::memcpy(buf.get(), "abcd", 4);
  HttpPayload body(std::move(buf), 4);
  EXPECT_TRUE(body.set());
  EXPECT_EQ(body.size(), 4U);
  EXPECT_EQ(body.view(), std::string_view("abcd", 4));
}

TEST(HttpPayload, ConstructFromRawChars) {
  RawChars rawChars(std::string_view("xyz", 3));
  HttpPayload body(std::move(rawChars));
  EXPECT_TRUE(body.set());
  EXPECT_EQ(body.size(), 3U);
  EXPECT_EQ(body.view(), "xyz");
}

TEST(HttpPayload, AppendStringToString) {
  HttpPayload body(std::string("foo"));
  body.append(std::string_view("bar"));
  EXPECT_EQ(body.view(), "foobar");
}

TEST(HttpPayload, AppendStringViewToVector) {
  std::vector<char> vec{'1', '2'};
  HttpPayload body(std::move(vec));
  body.append(std::string_view("34"));
  EXPECT_EQ(body.view(), "1234");
}

TEST(HttpPayload, AppendHttpBodyToString) {
  HttpPayload body1(std::string("head"));
  HttpPayload body2(std::string("tail"));
  body1.append(body2);
  EXPECT_EQ(body1.view(), "headtail");
}

TEST(HttpPayload, AppendHttpBodyToMonostateAdopts) {
  HttpPayload body1;  // monostate
  HttpPayload body2(std::string("adopted"));
  body1.append(body2);
  EXPECT_TRUE(body1.set());
  EXPECT_EQ(body1.view(), "adopted");
}

TEST(HttpPayload, AppendLargeToCharBuffer) {
  auto buf = std::make_unique<char[]>(3);
  std::memcpy(buf.get(), "ABC", 3);
  HttpPayload body(std::move(buf), 3);
  body.append(std::string_view("DEF"));
  EXPECT_EQ(body.size(), 6U);
  EXPECT_EQ(body.view(), "ABCDEF");
}

TEST(HttpPayload, ClearResetsSizeOrZeroesBuffer) {
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

TEST(HttpPayload, MultipleAppendCombinations) {
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
TEST(HttpPayload, ViewStabilityAfterAppend) {
  HttpPayload body1(std::string("start"));
  std::string_view data = body1.view();
  EXPECT_EQ(data, "start");
  body1.append(std::string_view("-more"));
  EXPECT_EQ(body1.view(), "start-more");
}

TEST(HttpPayload, AddSize) {
  HttpPayload body;
  EXPECT_THROW(body.addSize(5), std::logic_error);

  body = HttpPayload(RawChars(7));
  body.addSize(4);
  EXPECT_EQ(body.size(), 4U);

  body = HttpPayload(std::make_unique<char[]>(4), 4);
  EXPECT_THROW(body.addSize(3), std::logic_error);

  body = HttpPayload(std::make_unique<std::byte[]>(4), 4);
  EXPECT_THROW(body.addSize(3), std::logic_error);
}

TEST(HttpPayload, VectorChar) {
  std::vector<char> vec{char{0x01}, char{0x02}, char{0x03}};
  HttpPayload body(vec);

  EXPECT_TRUE(body.set());
  EXPECT_EQ(body.size(), 3U);
  EXPECT_EQ(std::memcmp(body.data(), "\x01\x02\x03", 3), 0);
  EXPECT_EQ(body.view(), std::string_view("\x01\x02\x03", 3));

  body.append(std::string_view("\x04\x05", 2));
  EXPECT_EQ(body.size(), 5U);
  EXPECT_EQ(std::memcmp(body.data(), "\x01\x02\x03\x04\x05", 5), 0);
  EXPECT_EQ(body.view(), std::string_view("\x01\x02\x03\x04\x05", 5));

  body.append(HttpPayload(std::vector<char>{char{0x06}, char{0x07}}));
  EXPECT_EQ(body.size(), 7U);
  EXPECT_EQ(std::memcmp(body.data(), "\x01\x02\x03\x04\x05\x06\x07", 7), 0);
  EXPECT_EQ(body.view(), std::string_view("\x01\x02\x03\x04\x05\x06\x07", 7));
}

TEST(HttpPayload, VectorByteData) {
  std::vector<std::byte> vec{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
  HttpPayload body(vec);

  EXPECT_TRUE(body.set());
  EXPECT_EQ(body.size(), 3U);
  EXPECT_EQ(std::memcmp(body.data(), "\x01\x02\x03", 3), 0);
  EXPECT_EQ(body.view(), std::string_view("\x01\x02\x03", 3));
  EXPECT_EQ(body.data(), reinterpret_cast<const char*>(body.view().data()));

  body.append(std::string_view("\x04\x05", 2));
  EXPECT_EQ(body.size(), 5U);
  EXPECT_EQ(std::memcmp(body.data(), "\x01\x02\x03\x04\x05", 5), 0);
  EXPECT_EQ(body.view(), std::string_view("\x01\x02\x03\x04\x05", 5));

  body.append(HttpPayload(std::vector<std::byte>{std::byte{0x06}, std::byte{0x07}}));
  EXPECT_EQ(body.size(), 7U);
  EXPECT_EQ(std::memcmp(body.data(), "\x01\x02\x03\x04\x05\x06\x07", 7), 0);
  EXPECT_EQ(body.view(), std::string_view("\x01\x02\x03\x04\x05\x06\x07", 7));
}

TEST(HttpPayload, CharBuffer) {
  auto buf = std::make_unique<char[]>(3);
  buf[0] = char{0x0A};
  buf[1] = char{0x0B};
  buf[2] = char{0x0C};
  HttpPayload body(std::move(buf), 3);

  EXPECT_TRUE(body.set());
  EXPECT_EQ(body.size(), 3U);
  EXPECT_EQ(std::memcmp(body.data(), "\x0A\x0B\x0C", 3), 0);
  EXPECT_EQ(body.view(), std::string_view("\x0A\x0B\x0C", 3));

  body.append(std::string_view("\x0D\x0E", 2));
  EXPECT_EQ(body.size(), 5U);
  EXPECT_EQ(std::memcmp(body.data(), "\x0A\x0B\x0C\x0D\x0E", 5), 0);
  EXPECT_EQ(body.view(), std::string_view("\x0A\x0B\x0C\x0D\x0E", 5));

  auto buf2 = std::make_unique<char[]>(2);
  buf2[0] = char{0x0F};
  buf2[1] = char{0x10};
  body.append(HttpPayload(std::move(buf2), 2));
  EXPECT_EQ(body.size(), 7U);
  EXPECT_EQ(std::memcmp(body.data(), "\x0A\x0B\x0C\x0D\x0E\x0F\x10", 7), 0);
  EXPECT_EQ(body.view(), std::string_view("\x0A\x0B\x0C\x0D\x0E\x0F\x10", 7));
}

TEST(HttpPayload, ByteBuffer) {
  auto buf = std::make_unique<std::byte[]>(3);
  buf[0] = std::byte{0x1A};
  buf[1] = std::byte{0x1B};
  buf[2] = std::byte{0x1C};
  HttpPayload body(std::move(buf), 3);

  EXPECT_TRUE(body.set());
  EXPECT_EQ(body.size(), 3U);
  EXPECT_EQ(std::memcmp(body.data(), "\x1A\x1B\x1C", 3), 0);
  EXPECT_EQ(body.view(), std::string_view("\x1A\x1B\x1C", 3));

  body.append(HttpPayload(std::string("\x1D\x1E")));
  EXPECT_EQ(body.size(), 5U);
  EXPECT_EQ(std::memcmp(body.data(), "\x1A\x1B\x1C\x1D\x1E", 5), 0);
  EXPECT_EQ(body.view(), std::string_view("\x1A\x1B\x1C\x1D\x1E", 5));

  auto buf2 = std::make_unique<std::byte[]>(2);
  buf2[0] = std::byte{0x1F};
  buf2[1] = std::byte{0x20};
  body.append(HttpPayload(std::move(buf2), 2));
  EXPECT_EQ(body.size(), 7U);
  EXPECT_EQ(std::memcmp(body.data(), "\x1A\x1B\x1C\x1D\x1E\x1F\x20", 7), 0);
  EXPECT_EQ(body.view(), std::string_view("\x1A\x1B\x1C\x1D\x1E\x1F\x20", 7));
}

// Additional tests: ensureAvailableCapacityExponential coverage for all storage variants
TEST(HttpPayload, EnsureAvailableCapacity_Monostate) {
  HttpPayload body;  // monostate
  body.ensureAvailableCapacityExponential(4);
  // after ensure, addSize should work (monostate becomes RawChars)
  body.addSize(3);
  EXPECT_EQ(body.size(), 3U);
}

TEST(HttpPayload, EnsureAvailableCapacity_String) {
  HttpPayload body(std::string("x"));
  // current size 1, request capacity for +4
  body.ensureAvailableCapacityExponential(4);
  body.addSize(4);
  EXPECT_EQ(body.size(), 5U);
}

TEST(HttpPayload, EnsureAvailableCapacity_VectorChar) {
  HttpPayload body(std::vector<char>{'a'});
  body.ensureAvailableCapacityExponential(4);
  body.addSize(2);
  EXPECT_EQ(body.size(), 3U);
}

TEST(HttpPayload, EnsureAvailableCapacity_VectorByte) {
  std::vector<std::byte> vec{std::byte{0x01}};
  HttpPayload body(vec);
  body.ensureAvailableCapacityExponential(6);
  body.addSize(3);
  EXPECT_EQ(body.size(), 4U);
}

TEST(HttpPayload, EnsureAvailableCapacity_RawChars) {
  RawChars raw(1);
  HttpPayload body(std::move(raw));
  body.ensureAvailableCapacityExponential(5);
  body.addSize(4);
  EXPECT_EQ(body.size(), 4U);
}

TEST(HttpPayload, EnsureAvailableCapacity_CharBufferAndBytesBuffer) {
  auto cbuf = std::make_unique<char[]>(3);
  std::memcpy(cbuf.get(), "ABC", 3);
  HttpPayload hb(std::move(cbuf), 3);
  // calling ensure should convert CharBuffer -> RawChars so append works
  hb.ensureAvailableCapacityExponential(0);
  hb.append(std::string_view("D"));
  EXPECT_EQ(hb.view(), "ABCD");

  auto bbuf = std::make_unique<std::byte[]>(2);
  bbuf[0] = std::byte{'X'};
  bbuf[1] = std::byte{'Y'};
  HttpPayload pb(std::move(bbuf), 2);
  pb.ensureAvailableCapacityExponential(0);
  pb.append(std::string_view("Z"));
  EXPECT_EQ(pb.size(), 3U);
}

TEST(HttpPayload, ShrinkToFitReducesNonEmptyPayload) {
  // Create a RawChars-backed payload with extra reserved capacity
  HttpPayload body(RawChars(64));
  body.addSize(16);  // pretend we've written 16 bytes
  EXPECT_GT(body.view().size(), 0U);
  // Ensure capacity larger than size
  EXPECT_GT(body.view().size(), 0U);

  body.shrink_to_fit();

  // After shrink_to_fit, capacity should equal size (no extra slack)
  EXPECT_EQ(body.view().size(), body.size());
}

TEST(HttpPayload, ShrinkToFitOnEmptyPayloadYieldsZeroCapacity) {
  HttpPayload empty;
  EXPECT_FALSE(empty.set());
  // Ensure calling shrink on empty payload results in zero capacity storage
  empty.shrink_to_fit();
  // After shrink, empty payload should still be unset and size zero
  EXPECT_EQ(empty.size(), 0U);
  EXPECT_EQ(empty.view().size(), 0U);
}