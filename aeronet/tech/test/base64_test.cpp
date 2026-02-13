#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "aeronet/base64-encode.hpp"

namespace aeronet {

namespace {

[[nodiscard]] inline std::string B64Encode(std::span<const char> binData) {
  std::string ret;
  ret.resize_and_overwrite(B64EncodedLen(binData.size()), [binData](char* out, std::size_t n) {
    ::aeronet::B64Encode(binData, out, static_cast<const char*>(out) + n);
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
  EXPECT_TRUE(encoded.find('+') != std::string::npos || encoded.find('/') != std::string::npos);
}

}  // namespace aeronet
