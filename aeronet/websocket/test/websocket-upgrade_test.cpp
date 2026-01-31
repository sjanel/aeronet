#include "aeronet/websocket-upgrade.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace aeronet {

namespace {
// Valid WebSocket key (24 base64 chars representing 16 bytes)
constexpr std::string_view kValidWebSocketKey = "dGhlIHNhbXBsZSBub25jZQ==";

// Expected Sec-WebSocket-Accept for the above key (computed per RFC 6455)
constexpr std::string_view kExpectedWebSocketAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
}  // namespace

TEST(UpgradeHandlerTest, IsValidWebSocketKey_ValidKey) {
  // Valid key: exactly 24 base64 characters
  EXPECT_TRUE(IsValidWebSocketKey(kValidWebSocketKey));
}

TEST(UpgradeHandlerTest, IsValidWebSocketKey_TooShort) { EXPECT_FALSE(IsValidWebSocketKey("dGhlIHNhbXBsZSBub25jZQ=")); }

TEST(UpgradeHandlerTest, IsValidWebSocketKey_TooLong) {
  EXPECT_FALSE(IsValidWebSocketKey("dGhlIHNhbXBsZSBub25jZQ==="));
}

TEST(UpgradeHandlerTest, IsValidWebSocketKey_InvalidCharacters) {
  EXPECT_FALSE(IsValidWebSocketKey("dGhlIHNhbXBsZSBub25j@Q=="));
}

TEST(UpgradeHandlerTest, IsValidWebSocketKey_Empty) { EXPECT_FALSE(IsValidWebSocketKey("")); }

TEST(UpgradeHandlerTest, IsValidWebSocketKey_NootEndingWithDoubleEquals) {
  EXPECT_FALSE(IsValidWebSocketKey("dGhlIHNhbXBsZSBub25jZQAA"));
}

TEST(UpgradeHandlerTest, IsValidWebSocketKey_ExactlyWrongPadding) {
  // Key with wrong padding position
  EXPECT_FALSE(IsValidWebSocketKey("dGhlIHNhbXBsZSBub25jZQA="));  // Single = at wrong position
  EXPECT_FALSE(IsValidWebSocketKey("dGhlIHNhbXBsZSBub25jZQ=A"));  // Single = at wrong position
}

TEST(UpgradeHandlerTest, IsValidWebSocketKey_NonBase64InMiddle) {
  EXPECT_FALSE(IsValidWebSocketKey("dGhlIHNhbXBsZS!ub25jZQ=="));  // ! is not base64
}

TEST(UpgradeHandlerTest, IsValidWebSocketKey_SpacesInKey) {
  EXPECT_FALSE(IsValidWebSocketKey("dGhlIHNhbXBsZS ub25jZQ=="));  // Space not allowed
}

// Test for ComputeWebSocketAccept
TEST(UpgradeHandlerTest, ComputeWebSocketAccept_RFC6455TestVector) {
  // RFC 6455 test vector
  const auto accept = ComputeWebSocketAccept(kValidWebSocketKey);

  EXPECT_EQ(std::string_view(accept.data(), accept.size()), kExpectedWebSocketAccept);
}
TEST(UpgradeHandlerTest, ComputeWebSocketAccept_EmptyKey) {
  const auto accept = ComputeWebSocketAccept("");
  // Should still work - concatenates empty string with GUID
  EXPECT_NE(accept[0], '\0');
}

TEST(UpgradeHandlerTest, ComputeWebSocketAccept_LongKey) {
  std::string longKey(1000, 'X');
  const auto accept = ComputeWebSocketAccept(longKey);
  EXPECT_NE(accept[0], '\0');
}

}  // namespace aeronet