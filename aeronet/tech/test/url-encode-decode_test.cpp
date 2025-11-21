#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "aeronet/raw-chars.hpp"
#include "aeronet/url-decode.hpp"
#include "aeronet/url-encode.hpp"

namespace aeronet {

// Helper predicate: unreserved characters per RFC 3986
struct IsUnreserved {
  bool operator()(char ch) const noexcept {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
           ch == '.' || ch == '~';
  }
};

namespace {
std::string encodeString(std::string_view input, const IsUnreserved &isUnreserved) {
  const std::size_t size = URLEncodedSize(input, isUnreserved);
  std::string out;
  out.resize(size);
  char *end = URLEncode(input, isUnreserved, out.data());
  // end should point just past the last written char
  EXPECT_EQ(end, out.data() + static_cast<std::ptrdiff_t>(size));
  return out;
}

// Overload accepting a generic predicate functor
template <class Pred>
std::string encodeString(std::string_view input, Pred pred) {
  const std::size_t size = URLEncodedSize(input, pred);
  std::string out;
  out.resize(size);
  char *end = URLEncode(input, pred, out.data());
  EXPECT_EQ(end, out.data() + static_cast<std::ptrdiff_t>(size));
  return out;
}

}  // namespace

TEST(UrlEncodeDecode, EncodeBasic) {
  std::string input = "ABC xyz";  // space must be encoded
  auto encoded = encodeString(input, IsUnreserved{});
  EXPECT_EQ(encoded, "ABC%20xyz");
}

TEST(UrlEncodeDecode, EncodeReserved) {
  std::string input = "!*'();:@&=+$,/?#[]";  // From RFC 3986 reserved set + others to ensure encoding
  auto encoded = encodeString(input, IsUnreserved{});
  // All should be percent encoded
  // We'll just verify no raw reserved characters remain except percent
  for (char ch : input) {
    if (ch != '%') {
      EXPECT_EQ(encoded.find(ch), std::string::npos) << "Character should have been encoded: " << ch;
    }
  }
}

TEST(UrlEncodeDecode, RoundTripSimple) {
  std::string original = "Hello-World_~";  // all unreserved
  auto encoded = encodeString(original, IsUnreserved{});
  EXPECT_EQ(encoded, original);  // no change
  RawChars copy(encoded);
  char *last = url::DecodeInPlace(copy.begin(), copy.end());
  EXPECT_NE(last, nullptr);
  EXPECT_EQ(std::string_view(copy), original);
}

TEST(UrlEncodeDecode, RoundTripWithSpaces) {
  std::string original = "Hello World";  // space encoded as %20
  auto encoded = encodeString(original, IsUnreserved{});
  EXPECT_EQ(encoded, "Hello%20World");
  RawChars copy(encoded);
  char *last = url::DecodeInPlace(copy.begin(), copy.end());
  EXPECT_NE(last, nullptr);
  EXPECT_EQ(std::string_view(copy.data(), last), original);
}

TEST(UrlEncodeDecode, PlusAsSpaceDecode) {
  std::string form = "Hello+World";
  RawChars formChars(form);
  char *last = url::DecodeInPlace(formChars.begin(), formChars.end(), ' ');
  EXPECT_NE(last, nullptr);
  EXPECT_EQ(std::string_view(formChars.data(), last), "Hello World");
}

TEST(UrlEncodeDecode, PercentLowerCaseHex) {
  // Lowercase hex should still decode
  RawChars lower("abc%2fdef");
  char *last = url::DecodeInPlace(lower.begin(), lower.end());
  EXPECT_NE(last, nullptr);
  EXPECT_EQ(std::string_view(lower.data(), last), "abc/def");
}

TEST(UrlEncodeDecode, InvalidPercentTooShort) {
  std::string bad = "abc%";  // truncated
  RawChars badChars(bad);
  char *last = url::DecodeInPlace(badChars.begin(), badChars.end());
  EXPECT_EQ(last, nullptr);
}

TEST(UrlEncodeDecode, InvalidPercentOneDigit) {
  std::string bad = "abc%2";  // only one hex digit
  RawChars badChars(bad);
  char *last = url::DecodeInPlace(badChars.begin(), badChars.end());
  EXPECT_EQ(last, nullptr);
}

TEST(UrlEncodeDecode, InvalidPercentNonHex) {
  std::string bad = "abc%2X";  // X not hex
  RawChars badChars(bad);
  char *last = url::DecodeInPlace(badChars.begin(), badChars.end());
  EXPECT_EQ(last, nullptr);
}

TEST(UrlEncodeDecode, UTF8RoundTrip) {
  // UTF-8 snowman + text
  std::string original = "\xE2\x98\x83 snow";  // ☃ snow
  auto encoded = encodeString(original, IsUnreserved{});
  RawChars copy2(encoded);
  char *last = url::DecodeInPlace(copy2.begin(), copy2.end());
  EXPECT_NE(last, nullptr);
  EXPECT_EQ(std::string_view(copy2.data(), last), original);
}

TEST(UrlEncodeDecode, MixedPlusAndPercent) {
  std::string input = "%2B+";  // %2B is '+'; plus should become space only if plusAsSpace
  RawChars decodedNoPlusSpaceChars(input);
  char *lastA = url::DecodeInPlace(decodedNoPlusSpaceChars.begin(), decodedNoPlusSpaceChars.end());
  EXPECT_NE(lastA, nullptr);
  EXPECT_EQ(std::string_view(decodedNoPlusSpaceChars.data(), lastA), "++");
  RawChars decodedPlusSpaceChars(input);
  char *lastB = url::DecodeInPlace(decodedPlusSpaceChars.begin(), decodedPlusSpaceChars.end(), ' ');
  EXPECT_NE(lastB, nullptr);
  EXPECT_EQ(std::string_view(decodedPlusSpaceChars.data(), lastB), "+ ");
}

TEST(UrlEncodeDecode, InPlaceDecodeBasic) {
  RawChars inputStr("Hello%20World");
  char *last = url::DecodeInPlace(inputStr.begin(), inputStr.end());
  EXPECT_NE(last, nullptr);
  EXPECT_EQ(std::string_view(inputStr.data(), last), "Hello World");
}

TEST(UrlEncodeDecode, InPlacePlusAsSpace) {
  RawChars inputStr("A+Plus+Sign");
  char *last = url::DecodeInPlace(inputStr.begin(), inputStr.end(), ' ');
  EXPECT_NE(last, nullptr);
  EXPECT_EQ(std::string_view(inputStr.data(), last), "A Plus Sign");
}

TEST(UrlEncodeDecode, InPlaceInvalid) {
  RawChars inputStr("Bad%G1");  // G invalid hex
  char *last = url::DecodeInPlace(inputStr.begin(), inputStr.end());
  EXPECT_EQ(last, nullptr);
}

TEST(UrlEncodeDecode, InPlaceUTF8) {
  RawChars original("\xE2\x98\x83");  // snowman
  // Encode it using existing encoder predicate (will encode all non-unreserved)
  struct IsUnreservedLocal {
    bool operator()(char ch) const noexcept {
      return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
             ch == '_' || ch == '.' || ch == '~';
    }
  };
  auto encoded = encodeString(std::string_view(original.data(), original.size()), IsUnreservedLocal{});
  RawChars copy(encoded);
  char *last = url::DecodeInPlace(copy.begin(), copy.end());
  EXPECT_NE(last, nullptr);
  EXPECT_EQ(std::string_view(copy.data(), last), std::string_view(original));
}

TEST(UrlEncodeDecode, EncodeUppercaseHexAndNonAscii) {
  // Ensure non-ASCII bytes and low-value bytes are percent-encoded using
  // uppercase hex digits and that unreserved characters remain unchanged.
  std::string input;
  input.push_back(static_cast<char>(0xFF));  // should encode as %FF
  input.push_back(static_cast<char>(0x01));  // should encode as %01
  input.push_back('A');                      // unreserved

  auto encoded = encodeString(input, IsUnreserved{});
  EXPECT_NE(encoded.find("%FF"), std::string::npos);
  EXPECT_NE(encoded.find("%01"), std::string::npos);
  EXPECT_EQ(encoded.back(), 'A');
}

TEST(UrlEncodeDecode, URLEncodedSizeMatchesOutput) {
  // Build a mixed input and verify URLEncodedSize equals the produced length
  // from URLEncode for a variety of bytes.
  std::string sample;
  // include a few representative bytes: alnum, space, reserved, non-ascii
  sample.push_back('x');
  sample.push_back(' ');
  sample.push_back('/');
  sample.push_back(static_cast<char>(0x80));
  sample.push_back('~');

  const std::size_t expectedSize = URLEncodedSize(sample, IsUnreserved{});
  auto out = encodeString(sample, IsUnreserved{});
  EXPECT_EQ(out.size(), expectedSize);
}

}  // namespace aeronet
