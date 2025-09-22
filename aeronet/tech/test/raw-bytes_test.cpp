#include "raw-bytes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <numeric>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "vector.hpp"

namespace aeronet {

using BytesView = std::span<const std::byte>;

namespace {
// Helper: create a vector<std::byte> from a string for iterator tests
vector<std::byte> toCharVec(std::string_view str) {
  return {reinterpret_cast<const std::byte *>(str.data()),
          reinterpret_cast<const std::byte *>(str.data()) + str.size()};
}

// Helper: produce a BytesView (span<const std::byte>) from any string-like view
BytesView toBytes(std::string_view sv) { return {reinterpret_cast<const std::byte *>(sv.data()), sv.size()}; }

}  // namespace

TEST(SimpleBufferTest, DefaultConstructor) {
  RawBytes<> buf;  // Explicit <> to avoid CTAD selecting the span constructor
  EXPECT_EQ(buf.size(), 0);
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.data(), nullptr);
  EXPECT_EQ(buf.begin(), buf.end());
  EXPECT_EQ(buf.data(), buf.data() + buf.size());
}

TEST(SimpleBufferTest, RandomAccessIteratorConstructor) {
  std::string text = "Hello";
  RawBytes<> buf(reinterpret_cast<const std::byte *>(text.data()),
                 reinterpret_cast<const std::byte *>(text.data()) + text.size());
  EXPECT_EQ(buf.size(), text.size());
  BytesView bufView(buf);
  auto textView = toBytes(text);
  EXPECT_TRUE(std::equal(bufView.data(), bufView.data() + bufView.size(), textView.data()));
}

TEST(SimpleBufferTest, MoveConstructor) {
  auto data = toCharVec("move-me");
  RawBytes<> buf1(data.begin(), data.end());
  auto *oldPtr = buf1.data();

  RawBytes<> buf2(std::move(buf1));
  EXPECT_EQ(buf2.data(), oldPtr);

  BytesView buf2View(buf2);
  BytesView dataView(data.data(), data.size());

  EXPECT_EQ(buf2.size(), data.size());
  EXPECT_TRUE(std::equal(buf2View.data(), buf2View.data() + buf2View.size(), dataView.data()));
}

TEST(SimpleBufferTest, MoveAssignment) {
  auto data = toCharVec("move-me");
  RawBytes<> buf1(data.begin(), data.end());
  auto *oldPtr = buf1.data();

  RawBytes<> buf2;
  buf2 = std::move(buf1);
  EXPECT_EQ(buf2.data(), oldPtr);

  BytesView buf2View(buf2);
  BytesView dataView(data.data(), data.size());

  EXPECT_EQ(buf2.size(), data.size());
  EXPECT_TRUE(std::equal(buf2View.data(), buf2View.data() + buf2View.size(), dataView.data()));
}

TEST(SimpleBufferTest, RangeForLoop) {
  std::string text = "range";
  RawBytes<> buf(reinterpret_cast<const std::byte *>(text.data()),
                 reinterpret_cast<const std::byte *>(text.data()) + text.size());
  std::string collected;
  for (auto ch : buf) {
    collected.push_back(static_cast<char>(ch));
  }
  EXPECT_EQ(collected, text);
}

TEST(SimpleBufferTest, RangesAlgorithmsWork) {
  vector<std::byte> text({std::byte('x'), std::byte('y'), std::byte('z')});
  RawBytes<> buf(text.begin(), text.end());

  // ranges::equal
  EXPECT_TRUE(std::ranges::equal(buf, text));
  // ranges::copy
  ASSERT_LE(buf.size(), static_cast<size_t>(std::numeric_limits<vector<std::byte>::size_type>::max()));
  vector<std::byte> copied(static_cast<vector<std::byte>::size_type>(buf.size()));
  std::ranges::copy(buf, copied.begin());
  std::ranges::copy(buf, copied.begin());
  EXPECT_EQ(copied, text);
}

}  // namespace aeronet