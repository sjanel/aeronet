#include "aeronet/base64url.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
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

// Returns the decoded bytes, or std::nullopt when the input is rejected.
[[nodiscard]] std::optional<std::string> Decode(std::string_view in) {
  std::string out;
  out.resize(B64UrlMaxDecodedLen(in.size()));
  std::size_t outLen = 0;
  if (!B64UrlDecode(in, out.data(), outLen)) {
    return std::nullopt;
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
  std::string all;
  all.resize(256);
  for (std::size_t i = 0; i < 256; ++i) {
    all[i] = static_cast<char>(i);
  }
  auto encoded = Encode(all);
  EXPECT_FALSE(encoded.contains('='));
  auto decoded = Decode(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, all);
}

TEST(Base64Url, DecodeRfcVectors) {
  EXPECT_EQ(Decode("").value(), "");
  EXPECT_EQ(Decode("Zg").value(), "f");
  EXPECT_EQ(Decode("Zm8").value(), "fo");
  EXPECT_EQ(Decode("Zm9v").value(), "foo");
  EXPECT_EQ(Decode("Zm9vYmFy").value(), "foobar");
}

TEST(Base64Url, DecodeToleratesPadding) {
  EXPECT_EQ(Decode("Zg==").value(), "f");
  EXPECT_EQ(Decode("Zm8=").value(), "fo");
}

TEST(Base64Url, DecodeRejectsInvalidAlphabet) {
  EXPECT_FALSE(Decode("Zg+v").has_value());  // '+' is standard base64, not base64url
  EXPECT_FALSE(Decode("Zg/v").has_value());  // '/' likewise
  EXPECT_FALSE(Decode("Z m8").has_value());  // space
}

TEST(Base64Url, DecodeRejectsTruncatedGroup) {
  // A single leftover character (mod 4 == 1) can never represent a whole byte.
  EXPECT_FALSE(Decode("Zm9vY").has_value());
}

TEST(Base64Url, DecodeAcceptsEveryAlphabetSymbol) {
  // Exercises each arm of the symbol→sextet decoder (upper, lower, digit, '-', '_').
  EXPECT_EQ(Decode("TQ").value(), "M");    // 'T','Q' upper-case
  EXPECT_EQ(Decode("bQ").value(), "m");    // lower-case
  EXPECT_EQ(Decode("MDk").value(), "09");  // digits
  EXPECT_TRUE(Decode("-_").has_value());   // '-' and '_'
}

}  // namespace aeronet
