#include "aeronet/base64url.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aeronet {

namespace {

[[nodiscard]] std::string Encode(std::string_view in) {
  std::string out;
  out.resize(B64UrlEncodedLen(in.size()));
  B64UrlEncode(std::span<const char>(in.data(), in.size()), out.data());
  return out;
}

// Returns the decoded bytes, or throw invalid_argument when the input is rejected.
std::string Decode(std::string_view in) {
  std::string out;
  out.resize(B64UrlMaxDecodedLen(in.size()));
  std::size_t outLen = 0;
  if (!B64UrlDecode(in, out.data(), outLen)) {
    throw std::invalid_argument("Invalid base64url input");
  }
  out.resize(outLen);
  return out;
}

}  // namespace

// RFC 4648 §10 test vectors, base64url-flavoured (no padding).
TEST(Base64Url, EncodeRfcVectors) {
  EXPECT_EQ(Encode(""), "");
  EXPECT_EQ(Encode("f"), "Zg");
  EXPECT_EQ(Encode("fo"), "Zm8");
  EXPECT_EQ(Encode("foo"), "Zm9v");
  EXPECT_EQ(Encode("foob"), "Zm9vYg");
  EXPECT_EQ(Encode("fooba"), "Zm9vYmE");
  EXPECT_EQ(Encode("foobar"), "Zm9vYmFy");
}

TEST(Base64Url, UsesUrlSafeAlphabet) {
  // Bytes 0xFB 0xEF 0xBE encode to "++++" / "////" worth of bits in standard base64; base64url
  // must instead surface '-' and '_' and never '+' or '/'.
  const std::array<char, 3> data{static_cast<char>(0xFB), static_cast<char>(0xEF), static_cast<char>(0xBE)};
  auto encoded = Encode(std::string_view(data.data(), data.size()));
  EXPECT_EQ(encoded, "----");  // standard base64 would yield "++++"
  EXPECT_FALSE(encoded.contains('+'));
  EXPECT_FALSE(encoded.contains('/'));
  EXPECT_FALSE(encoded.contains('='));
}

TEST(Base64Url, RoundTripAllByteValues) {
  std::string all(256, '\0');
  for (std::size_t i = 0; i < all.size(); ++i) {
    all[i] = static_cast<char>(i);
  }
  auto encoded = Encode(all);
  EXPECT_FALSE(encoded.contains('='));
  auto decoded = Decode(encoded);
  EXPECT_EQ(decoded, all);
}

TEST(Base64Url, DecodeRfcVectors) {
  EXPECT_EQ(Decode(""), "");
  EXPECT_EQ(Decode("Zg"), "f");
  EXPECT_EQ(Decode("Zm8"), "fo");
  EXPECT_EQ(Decode("Zm9v"), "foo");
  EXPECT_EQ(Decode("Zm9vYmFy"), "foobar");
}

TEST(Base64Url, DecodeToleratesPadding) {
  EXPECT_EQ(Decode("Zg=="), "f");
  EXPECT_EQ(Decode("Zm8="), "fo");
}

TEST(Base64Url, DecodeRejectsInvalidAlphabet) {
  EXPECT_THROW(Decode("Zg+v"), std::invalid_argument);  // '+' is standard base64, not base64url
  EXPECT_THROW(Decode("Zg/v"), std::invalid_argument);  // '/' likewise
  EXPECT_THROW(Decode("Z m8"), std::invalid_argument);  // space
}

TEST(Base64Url, DecodeRejectsTruncatedGroup) {
  // A single leftover character (mod 4 == 1) can never represent a whole byte.
  EXPECT_THROW(Decode("Zm9vY"), std::invalid_argument);
}

TEST(Base64Url, DecodeAcceptsEveryAlphabetSymbol) {
  // Exercises each arm of the symbol→sextet decoder (upper, lower, digit, '-', '_').
  EXPECT_EQ(Decode("TQ"), "M");     // 'T','Q' upper-case
  EXPECT_EQ(Decode("bQ"), "m");     // lower-case
  EXPECT_EQ(Decode("MDk"), "09");   // digits
  EXPECT_EQ(Decode("-_"), "\xFB");  // '-' and '_'
}

}  // namespace aeronet
