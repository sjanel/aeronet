#include "aeronet/base64.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aeronet {

namespace {

[[nodiscard]] inline std::string B64Encode(std::span<const char> binData) {
  std::string ret;
  ret.resize_and_overwrite(B64EncodedLen(binData.size()), [binData](char* out, std::size_t n) {
    ::aeronet::B64Encode(std::string_view(binData.data(), binData.size()), out, static_cast<const char*>(out) + n);
    return n;
  });
  return ret;
}
std::string B64Encode(const char*) = delete;

}  // namespace

TEST(Base64, EncodeEmpty) { EXPECT_EQ(B64Encode(std::string_view("")), ""); }
TEST(Base64, Encode1) { EXPECT_EQ(B64Encode(std::string_view("f")), "Zg=="); }
TEST(Base64, Encode2) { EXPECT_EQ(B64Encode(std::string_view("fo")), "Zm8="); }
TEST(Base64, Encode3) { EXPECT_EQ(B64Encode(std::string_view("foo")), "Zm9v"); }
TEST(Base64, Encode4) { EXPECT_EQ(B64Encode(std::string_view("foob")), "Zm9vYg=="); }
TEST(Base64, Encode5) { EXPECT_EQ(B64Encode(std::string_view("fooba")), "Zm9vYmE="); }
TEST(Base64, Encode6) { EXPECT_EQ(B64Encode(std::string_view("foobar")), "Zm9vYmFy"); }
TEST(Base64, Encode7) { EXPECT_EQ(B64Encode(std::string_view("foobarz")), "Zm9vYmFyeg=="); }
TEST(Base64, Encode8) { EXPECT_EQ(B64Encode(std::string_view("foobarzY")), "Zm9vYmFyelk="); }
TEST(Base64, Encode9) { EXPECT_EQ(B64Encode(std::string_view("foobarzYg")), "Zm9vYmFyelln"); }

// Test encoding binary data with various bytes
TEST(Base64, EncodeBinaryData) {
  std::array<char, 3> binary = {'\x00', '\xFF', '\x7F'};
  std::string_view binaryView(binary.data(), binary.size());
  auto encoded = B64Encode(binaryView);
  EXPECT_EQ(encoded, "AP9/");
}

// Test the std::array overload of B64Encode
TEST(Base64, EncodeStdArray) {
  std::array<char, 3> data = {'a', 'b', 'c'};
  auto result = B64Encode(data);
  EXPECT_EQ(result.size(), 4);  // 3 bytes -> 4 base64 chars (no padding)
  EXPECT_EQ(std::string_view(result.data(), result.size()), "YWJj");
}

// Test encoding special base64 characters + and /
TEST(Base64, EncodeSpecialChars) {
  // Binary data that produces + and / in output
  std::array<char, 3> data = {'\xFB', '\xEF', '\xBE'};
  std::string_view input(data.data(), data.size());
  auto encoded = B64Encode(input);
  EXPECT_TRUE(encoded.contains('+') || encoded.contains('/'));
}

namespace {

[[nodiscard]] std::string Encode(std::string_view in) {
  std::string out;
  out.resize(B64UrlEncodedLen(in.size()));
  B64UrlEncode(in, out.data());
  return out;
}

// Returns the decoded bytes, or throw invalid_argument when the input is rejected.
std::string Decode(std::string_view in) {
  std::string out;
  out.resize(B64UrlMaxDecodedLen(in.size()));
  std::size_t outLen = 0;
  outLen = B64UrlDecode(in, out.data());
  if (outLen + 1UL == 0) {
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
