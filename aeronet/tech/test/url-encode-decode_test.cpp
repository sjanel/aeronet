#include <gtest/gtest.h>

#include <span>
#include <string>
#include <string_view>

#include "raw-chars.hpp"
#include "url-decode.hpp"
#include "url-encode.hpp"

namespace aeronet {

// Helper predicate: unreserved characters per RFC 3986
struct IsUnreserved {
  bool operator()(char ch) const noexcept {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
           ch == '.' || ch == '~';
  }
};

TEST(UrlEncodeDecode, EncodeBasic) {
  std::string input = "ABC xyz";  // space must be encoded
  auto encoded = URLEncode(std::span<const char>(input.data(), input.size()), IsUnreserved{});
  EXPECT_EQ(encoded, "ABC%20xyz");
}

TEST(UrlEncodeDecode, EncodeReserved) {
  std::string input = "!*'();:@&=+$,/?#[]";  // From RFC 3986 reserved set + others to ensure encoding
  auto encoded = URLEncode(std::span<const char>(input.data(), input.size()), IsUnreserved{});
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
  auto encoded = URLEncode(std::span<const char>(original.data(), original.size()), IsUnreserved{});
  EXPECT_EQ(encoded, original);  // no change
  RawChars copy(encoded);
  bool okDecode = URLDecodeInPlace(copy, false);
  EXPECT_TRUE(okDecode);
  EXPECT_EQ(std::string_view(copy), original);
}

TEST(UrlEncodeDecode, RoundTripWithSpaces) {
  std::string original = "Hello World";  // space encoded as %20
  auto encoded = URLEncode(std::span<const char>(original.data(), original.size()), IsUnreserved{});
  EXPECT_EQ(encoded, "Hello%20World");
  RawChars copy(encoded);
  bool okDecode = URLDecodeInPlace(copy, false);
  EXPECT_TRUE(okDecode);
  EXPECT_EQ(std::string_view(copy), original);
}

TEST(UrlEncodeDecode, PlusAsSpaceDecode) {
  std::string form = "Hello+World";
  RawChars formChars(form);
  bool okDecode = URLDecodeInPlace(formChars, true);
  EXPECT_TRUE(okDecode);
  EXPECT_EQ(std::string_view(formChars), "Hello World");
}

TEST(UrlEncodeDecode, PercentLowerCaseHex) {
  // Lowercase hex should still decode
  RawChars lower("abc%2fdef");
  bool okDecode = URLDecodeInPlace(lower, false);
  EXPECT_TRUE(okDecode);
  EXPECT_EQ(std::string_view(lower), "abc/def");
}

TEST(UrlEncodeDecode, InvalidPercentTooShort) {
  std::string bad = "abc%";  // truncated
  RawChars badChars(bad);
  bool ok = URLDecodeInPlace(badChars, false);
  EXPECT_FALSE(ok);
}

TEST(UrlEncodeDecode, InvalidPercentOneDigit) {
  std::string bad = "abc%2";  // only one hex digit
  RawChars badChars(bad);
  bool ok = URLDecodeInPlace(badChars, false);
  EXPECT_FALSE(ok);
}

TEST(UrlEncodeDecode, InvalidPercentNonHex) {
  std::string bad = "abc%2X";  // X not hex
  RawChars badChars(bad);
  bool ok = URLDecodeInPlace(badChars, false);
  EXPECT_FALSE(ok);
}

TEST(UrlEncodeDecode, UTF8RoundTrip) {
  // UTF-8 snowman + text
  std::string original = "\xE2\x98\x83 snow";  // ☃ snow
  auto encoded = URLEncode(std::span<const char>(original.data(), original.size()), IsUnreserved{});
  RawChars copy2(encoded);
  bool okDecode2 = URLDecodeInPlace(copy2, false);
  EXPECT_TRUE(okDecode2);
  EXPECT_EQ(std::string_view(copy2), original);
}

TEST(UrlEncodeDecode, MixedPlusAndPercent) {
  std::string input = "%2B+";  // %2B is '+'; plus should become space only if plusAsSpace
  RawChars decodedNoPlusSpaceChars(input);
  bool okA = URLDecodeInPlace(decodedNoPlusSpaceChars, false);
  EXPECT_TRUE(okA);
  EXPECT_EQ(std::string_view(decodedNoPlusSpaceChars), "++");
  RawChars decodedPlusSpaceChars(input);
  bool okB = URLDecodeInPlace(decodedPlusSpaceChars, true);
  EXPECT_TRUE(okB);
  EXPECT_EQ(std::string_view(decodedPlusSpaceChars), "+ ");
}

TEST(UrlEncodeDecode, InPlaceDecodeBasic) {
  RawChars inputStr("Hello%20World");
  bool ok = URLDecodeInPlace(inputStr, false);
  EXPECT_TRUE(ok);
  EXPECT_EQ(std::string_view(inputStr), "Hello World");
}

TEST(UrlEncodeDecode, InPlacePlusAsSpace) {
  RawChars inputStr("A+Plus+Sign");
  bool ok = URLDecodeInPlace(inputStr, true);
  EXPECT_TRUE(ok);
  EXPECT_EQ(std::string_view(inputStr), "A Plus Sign");
}

TEST(UrlEncodeDecode, InPlaceInvalid) {
  RawChars inputStr("Bad%G1");  // G invalid hex
  bool ok = URLDecodeInPlace(inputStr, false);
  EXPECT_FALSE(ok);
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
  auto encoded = URLEncode(std::span<const char>(original.data(), original.size()), IsUnreservedLocal{});
  RawChars copy(encoded);
  bool ok = URLDecodeInPlace(copy, false);
  EXPECT_TRUE(ok);
  EXPECT_EQ(std::string_view(copy), std::string_view(original));
}

}  // namespace aeronet
