#include "aeronet/websocket-frame.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/raw-bytes.hpp"
#include "aeronet/websocket-constants.hpp"

namespace aeronet::websocket {
namespace {

inline std::span<const std::byte> sv_bytes(std::string_view sv) noexcept {
  return std::as_bytes(std::span<const char>(sv.data(), sv.size()));
}

inline std::span<const std::byte> sv_bytes(const char *ptr, std::size_t n) noexcept {
  return std::as_bytes(std::span<const char>(ptr, n));
}

inline std::span<const std::byte> buf_bytes(const RawBytes &buf) noexcept { return {buf.data(), buf.size()}; }

inline std::span<const std::byte> buf_bytes_partial(const RawBytes &buf, std::size_t n) noexcept {
  return {buf.data(), n};
}

template <typename Container>
inline std::span<const std::byte> container_bytes(const Container &cont) noexcept {
  return std::span<const std::byte>(reinterpret_cast<const std::byte *>(cont.data()), cont.size());
}
}  // namespace

class WebSocketFrameTest : public ::testing::Test {
 protected:
  RawBytes buffer;
};

// ============================================================================
// FrameHeader::headerSize tests
// ============================================================================

TEST_F(WebSocketFrameTest, HeaderSizeUnmaskedSmallPayload) {
  FrameHeader header{};
  header.masked = false;
  header.payloadLength = 100;  // < 126
  EXPECT_EQ(header.headerSize(), 2);
}

TEST_F(WebSocketFrameTest, HeaderSizeMaskedSmallPayload) {
  FrameHeader header{};
  header.masked = true;
  header.payloadLength = 100;
  EXPECT_EQ(header.headerSize(), 6);  // 2 + 4 (mask)
}

TEST_F(WebSocketFrameTest, HeaderSizeUnmasked16BitLength) {
  FrameHeader header{};
  header.masked = false;
  header.payloadLength = 1000;        // >= 126 and < 65536
  EXPECT_EQ(header.headerSize(), 4);  // 2 + 2 (extended length)
}

TEST_F(WebSocketFrameTest, HeaderSizeMasked16BitLength) {
  FrameHeader header{};
  header.masked = true;
  header.payloadLength = 1000;
  EXPECT_EQ(header.headerSize(), 8);  // 2 + 2 + 4 (mask)
}

TEST_F(WebSocketFrameTest, HeaderSizeUnmasked64BitLength) {
  FrameHeader header{};
  header.masked = false;
  header.payloadLength = 100000;       // >= 65536
  EXPECT_EQ(header.headerSize(), 10);  // 2 + 8 (extended length)
}

TEST_F(WebSocketFrameTest, HeaderSizeMasked64BitLength) {
  FrameHeader header{};
  header.masked = true;
  header.payloadLength = 100000;
  EXPECT_EQ(header.headerSize(), 14);  // 2 + 8 + 4 (mask)
}

// ============================================================================
// BuildFrame tests
// ============================================================================

TEST_F(WebSocketFrameTest, BuildUnmaskedTextFrame) {
  std::string_view payload = "Hello";
  BuildFrame(buffer, Opcode::Text, sv_bytes(payload));

  ASSERT_GE(buffer.size(), 7);  // 2 header + 5 payload

  auto ptr = reinterpret_cast<const uint8_t *>(buffer.data());
  // Byte 0: FIN=1, RSV=000, opcode=0001
  EXPECT_EQ(ptr[0], 0x81);
  // Byte 1: MASK=0, length=5
  EXPECT_EQ(ptr[1], 0x05);
  // Payload
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(ptr + 2), 5), "Hello");
}

TEST_F(WebSocketFrameTest, BuildMaskedTextFrame) {
  std::string_view payload = "Hi";
  MaskingKey mask = {std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
  BuildFrame(buffer, Opcode::Text, sv_bytes(payload), true, true, mask);

  ASSERT_GE(buffer.size(), 8);  // 2 header + 4 mask + 2 payload

  auto ptr = reinterpret_cast<const uint8_t *>(buffer.data());
  // Byte 0: FIN=1, RSV=000, opcode=0001
  EXPECT_EQ(ptr[0], 0x81);
  // Byte 1: MASK=1, length=2
  EXPECT_EQ(ptr[1], 0x82);
  // Masking key
  EXPECT_EQ(ptr[2], 0x12);
  EXPECT_EQ(ptr[3], 0x34);
  EXPECT_EQ(ptr[4], 0x56);
  EXPECT_EQ(ptr[5], 0x78);
  // Payload is masked: 'H' ^ 0x12, 'i' ^ 0x34
  EXPECT_EQ(ptr[6], static_cast<uint8_t>('H') ^ 0x12);
  EXPECT_EQ(ptr[7], static_cast<uint8_t>('i') ^ 0x34);
}

TEST_F(WebSocketFrameTest, BuildBinaryFrame) {
  std::array<uint8_t, 3> payload = {0xDE, 0xAD, 0xBE};
  BuildFrame(buffer, Opcode::Binary, container_bytes(payload));

  ASSERT_GE(buffer.size(), 5);
  auto ptr = reinterpret_cast<const uint8_t *>(buffer.data());
  EXPECT_EQ(ptr[0], 0x82);  // FIN=1, opcode=binary
  EXPECT_EQ(ptr[1], 0x03);
  EXPECT_EQ(ptr[2], 0xDE);
  EXPECT_EQ(ptr[3], 0xAD);
  EXPECT_EQ(ptr[4], 0xBE);
}

TEST_F(WebSocketFrameTest, BuildFragmentedFrame) {
  std::string_view payload = "Test";
  // First fragment: FIN=0
  BuildFrame(buffer, Opcode::Text, sv_bytes(payload), false);

  auto ptr = reinterpret_cast<const uint8_t *>(buffer.data());
  // FIN=0
  EXPECT_EQ(ptr[0] & 0x80, 0x00);
  EXPECT_EQ(ptr[0] & 0x0F, static_cast<uint8_t>(Opcode::Text));
}

TEST_F(WebSocketFrameTest, BuildContinuationFrame) {
  std::string_view payload = "More";
  BuildFrame(buffer, Opcode::Continuation, sv_bytes(payload), true);

  auto ptr = reinterpret_cast<const uint8_t *>(buffer.data());
  EXPECT_EQ(ptr[0], 0x80);  // FIN=1, opcode=continuation
}

TEST_F(WebSocketFrameTest, BuildPingFrame) {
  std::string_view payload = "ping";
  BuildFrame(buffer, Opcode::Ping, sv_bytes(payload));

  auto ptr = reinterpret_cast<const uint8_t *>(buffer.data());
  EXPECT_EQ(ptr[0], 0x89);  // FIN=1, opcode=ping
}

TEST_F(WebSocketFrameTest, BuildPongFrame) {
  std::string_view payload = "pong";
  BuildFrame(buffer, Opcode::Pong, sv_bytes(payload));

  auto ptr = reinterpret_cast<const uint8_t *>(buffer.data());
  EXPECT_EQ(ptr[0], 0x8A);  // FIN=1, opcode=pong
}

TEST_F(WebSocketFrameTest, Build16BitLengthFrame) {
  // Payload of 200 bytes (requires 16-bit extended length)
  std::array<uint8_t, 200> payload{};
  BuildFrame(buffer, Opcode::Binary, container_bytes(payload));

  ASSERT_GE(buffer.size(), 204);  // 4 header + 200 payload
  auto ptr = reinterpret_cast<const uint8_t *>(buffer.data());
  EXPECT_EQ(ptr[0], 0x82);  // FIN=1, opcode=binary
  EXPECT_EQ(ptr[1], 126);   // Extended 16-bit length marker
  // Length in big-endian
  EXPECT_EQ(ptr[2], 0x00);
  EXPECT_EQ(ptr[3], 0xC8);  // 200
}

// ============================================================================
// BuildCloseFrame tests
// ============================================================================

TEST_F(WebSocketFrameTest, BuildCloseFrameWithCodeAndReason) {
  BuildCloseFrame(buffer, CloseCode::Normal, "Normal Closure");

  ASSERT_GE(buffer.size(), 18);  // 2 header + 2 code + 14 reason
  auto ptr = reinterpret_cast<const uint8_t *>(buffer.data());
  EXPECT_EQ(ptr[0], 0x88);  // FIN=1, opcode=close
  EXPECT_EQ(ptr[1], 16);    // 2 + 14
  // Status code in big-endian (1000)
  EXPECT_EQ(ptr[2], 0x03);
  EXPECT_EQ(ptr[3], 0xE8);
  // Reason
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(ptr + 4), 14), "Normal Closure");
}

TEST_F(WebSocketFrameTest, BuildCloseFrameNoReason) {
  BuildCloseFrame(buffer, CloseCode::GoingAway, "");

  ASSERT_GE(buffer.size(), 4);  // 2 header + 2 code
  auto ptr = reinterpret_cast<const uint8_t *>(buffer.data());
  EXPECT_EQ(ptr[0], 0x88);
  EXPECT_EQ(ptr[1], 2);
  EXPECT_EQ(ptr[2], 0x03);
  EXPECT_EQ(ptr[3], 0xE9);  // 1001
}

TEST_F(WebSocketFrameTest, BuildMaskedCloseFrame) {
  MaskingKey mask = {std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD}};
  BuildCloseFrame(buffer, CloseCode::Normal, "", true, mask);

  auto ptr = reinterpret_cast<const uint8_t *>(buffer.data());
  EXPECT_EQ(ptr[0], 0x88);
  EXPECT_EQ(ptr[1], 0x82);  // MASK=1, length=2
  // Masking key
  EXPECT_EQ(ptr[2], 0xAA);
  EXPECT_EQ(ptr[3], 0xBB);
  EXPECT_EQ(ptr[4], 0xCC);
  EXPECT_EQ(ptr[5], 0xDD);
}

// ============================================================================
// ParseFrame tests
// ============================================================================

TEST_F(WebSocketFrameTest, ParseIncompleteHeader) {
  std::array<std::byte, 1> data = {std::byte{0x81}};
  auto result = ParseFrame(data);
  EXPECT_EQ(result.status, FrameParseResult::Status::Incomplete);
}

TEST_F(WebSocketFrameTest, ParseIncompleteExtendedLength) {
  // 16-bit length marker but only 1 extra byte
  // Use client-side parsing (isServerSide=false) to avoid mask validation
  std::array<std::byte, 3> data = {std::byte{0x81}, std::byte{126}, std::byte{0x00}};
  auto result = ParseFrame(data, 0, false);
  EXPECT_EQ(result.status, FrameParseResult::Status::Incomplete);
}

TEST_F(WebSocketFrameTest, ParseUnmaskedTextFrame) {
  // Build a frame first
  std::string_view payload = "Hello";
  BuildFrame(buffer, Opcode::Text, sv_bytes(payload));

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, false);

  EXPECT_EQ(result.status, FrameParseResult::Status::Complete);
  EXPECT_EQ(result.header.opcode, Opcode::Text);
  EXPECT_TRUE(result.header.fin);
  EXPECT_FALSE(result.header.masked);
  EXPECT_EQ(result.header.payloadLength, 5);
  EXPECT_EQ(result.payload.size(), 5);
}

TEST_F(WebSocketFrameTest, ParseMaskedTextFrame) {
  MaskingKey mask = {std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
  std::string_view payload = "Hi";
  BuildFrame(buffer, Opcode::Text, sv_bytes(payload), true, true, mask);

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, true);

  EXPECT_EQ(result.status, FrameParseResult::Status::Complete);
  EXPECT_EQ(result.header.opcode, Opcode::Text);
  EXPECT_TRUE(result.header.masked);
  EXPECT_EQ(result.header.payloadLength, 2);
}

TEST_F(WebSocketFrameTest, ParseServerRejectsUnmaskedClient) {
  // Server-side parsing should reject unmasked frames from client
  std::string_view payload = "Test";
  BuildFrame(buffer, Opcode::Text, sv_bytes(payload), true, false);

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, true);  // Server-side

  EXPECT_EQ(result.status, FrameParseResult::Status::ProtocolError);
}

TEST_F(WebSocketFrameTest, ParseClientAcceptsUnmasked) {
  // Client-side parsing should accept unmasked frames from server
  std::string_view payload = "Test";
  BuildFrame(buffer, Opcode::Text, sv_bytes(payload), true, false);

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, false);  // Client-side

  EXPECT_EQ(result.status, FrameParseResult::Status::Complete);
}

TEST_F(WebSocketFrameTest, ParsePayloadTooLarge) {
  // Create frame with length exceeding limit
  std::array<std::byte, 10> data;
  data[0] = std::byte{0x82};  // FIN=1, opcode=binary
  data[1] = std::byte{127};   // 64-bit length marker
  // Set length to very large value
  data[2] = std::byte{0x00};
  data[3] = std::byte{0x00};
  data[4] = std::byte{0x00};
  data[5] = std::byte{0x01};  // 16MB+
  data[6] = std::byte{0x00};
  data[7] = std::byte{0x00};
  data[8] = std::byte{0x00};
  data[9] = std::byte{0x00};

  auto result = ParseFrame(data, 1024UL * 1024UL, false);  // 1MB limit
  EXPECT_EQ(result.status, FrameParseResult::Status::PayloadTooLarge);
}

TEST_F(WebSocketFrameTest, ParseCloseFrame) {
  BuildCloseFrame(buffer, CloseCode::Normal, "bye");

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, false);

  EXPECT_EQ(result.status, FrameParseResult::Status::Complete);
  EXPECT_EQ(result.header.opcode, Opcode::Close);
  EXPECT_TRUE(result.header.fin);
}

TEST_F(WebSocketFrameTest, ParsePingFrame) {
  BuildFrame(buffer, Opcode::Ping, sv_bytes("test", 4));

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, false);

  EXPECT_EQ(result.status, FrameParseResult::Status::Complete);
  EXPECT_EQ(result.header.opcode, Opcode::Ping);
}

TEST_F(WebSocketFrameTest, Parse16BitLength) {
  std::array<uint8_t, 200> payload;
  std::ranges::fill(payload, 0x42);
  BuildFrame(buffer, Opcode::Binary, container_bytes(payload));

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, false);

  EXPECT_EQ(result.status, FrameParseResult::Status::Complete);
  EXPECT_EQ(result.header.payloadLength, 200);
  EXPECT_EQ(result.payload.size(), 200);
}

TEST_F(WebSocketFrameTest, ParseIncompletePayload) {
  std::string_view payload = "Hello World!";
  BuildFrame(buffer, Opcode::Text, sv_bytes(payload));

  // Only provide half the frame
  auto data = buf_bytes_partial(buffer, buffer.size() / 2);
  auto result = ParseFrame(data, 0, false);

  EXPECT_EQ(result.status, FrameParseResult::Status::Incomplete);
}

// ============================================================================
// ApplyMask tests
// ============================================================================

TEST_F(WebSocketFrameTest, ApplyMaskBasic) {
  std::array<std::byte, 4> data = {std::byte{'A'}, std::byte{'B'}, std::byte{'C'}, std::byte{'D'}};
  MaskingKey mask = {std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};

  ApplyMask(data, mask);

  EXPECT_EQ(data[0], std::byte{static_cast<uint8_t>('A') ^ 0x12});
  EXPECT_EQ(data[1], std::byte{static_cast<uint8_t>('B') ^ 0x34});
  EXPECT_EQ(data[2], std::byte{static_cast<uint8_t>('C') ^ 0x56});
  EXPECT_EQ(data[3], std::byte{static_cast<uint8_t>('D') ^ 0x78});
}

TEST_F(WebSocketFrameTest, ApplyMaskReversible) {
  std::string original = "The quick brown fox";
  std::array<std::byte, 19> data;
  std::memcpy(data.data(), original.data(), 19);
  MaskingKey mask = {std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF}, std::byte{0x01}};

  ApplyMask(data, mask);  // Mask
  ApplyMask(data, mask);  // Unmask

  EXPECT_EQ(std::memcmp(data.data(), original.data(), 19), 0);
}

TEST_F(WebSocketFrameTest, ApplyMaskLargeData) {
  // Test 64-bit optimization path
  std::array<std::byte, 1024> data;
  std::ranges::fill(data, std::byte{0xFF});
  MaskingKey mask = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};

  std::array<std::byte, 1024> backup = data;

  ApplyMask(data, mask);

  // Verify mask was applied (data should be different)
  EXPECT_NE(std::memcmp(data.data(), backup.data(), 1024), 0);

  // Unmask and verify original
  ApplyMask(data, mask);
  EXPECT_EQ(std::memcmp(data.data(), backup.data(), 1024), 0);
}

TEST_F(WebSocketFrameTest, ApplyMaskEmpty) {
  std::span<std::byte> empty;
  MaskingKey mask = {std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};

  // Should not crash
  ApplyMask(empty, mask);
}

// ============================================================================
// ParseClosePayload tests
// ============================================================================

TEST_F(WebSocketFrameTest, ParseClosePayloadWithReason) {
  BuildCloseFrame(buffer, CloseCode::GoingAway, "Going Away");

  auto data = buf_bytes(buffer);
  auto frameResult = ParseFrame(data, 0, false);
  ASSERT_EQ(frameResult.status, FrameParseResult::Status::Complete);

  auto closePayload = ParseClosePayload(frameResult.payload);
  EXPECT_EQ(closePayload.code, CloseCode::GoingAway);
  EXPECT_EQ(closePayload.reason, "Going Away");
}

TEST_F(WebSocketFrameTest, ParseClosePayloadNoReason) {
  BuildCloseFrame(buffer, CloseCode::ProtocolError, "");

  auto data = buf_bytes(buffer);
  auto frameResult = ParseFrame(data, 0, false);
  ASSERT_EQ(frameResult.status, FrameParseResult::Status::Complete);

  auto closePayload = ParseClosePayload(frameResult.payload);
  EXPECT_EQ(closePayload.code, CloseCode::ProtocolError);
  EXPECT_TRUE(closePayload.reason.empty());
}

TEST_F(WebSocketFrameTest, ParseClosePayloadEmpty) {
  // Empty close frame (no code or reason)
  std::span<const std::byte> empty;
  auto closePayload = ParseClosePayload(empty);
  EXPECT_EQ(closePayload.code, CloseCode::NoStatusReceived);
  EXPECT_TRUE(closePayload.reason.empty());
}

TEST_F(WebSocketFrameTest, ParseClosePayloadOnlyCode) {
  // Manually create payload with just code
  std::array<std::byte, 2> payload = {std::byte{0x03}, std::byte{0xE8}};  // 1000
  auto closePayload = ParseClosePayload(payload);
  EXPECT_EQ(closePayload.code, CloseCode::Normal);
  EXPECT_TRUE(closePayload.reason.empty());
}

// ============================================================================
// Opcode tests
// ============================================================================

TEST_F(WebSocketFrameTest, OpcodeValues) {
  EXPECT_EQ(static_cast<uint8_t>(Opcode::Continuation), 0x00);
  EXPECT_EQ(static_cast<uint8_t>(Opcode::Text), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(Opcode::Binary), 0x02);
  EXPECT_EQ(static_cast<uint8_t>(Opcode::Close), 0x08);
  EXPECT_EQ(static_cast<uint8_t>(Opcode::Ping), 0x09);
  EXPECT_EQ(static_cast<uint8_t>(Opcode::Pong), 0x0A);
}

// ============================================================================
// Constants tests
// ============================================================================

TEST_F(WebSocketFrameTest, DefaultMaxMessageSize) { EXPECT_EQ(kDefaultMaxMessageSize, 64 * 1024 * 1024); }

TEST_F(WebSocketFrameTest, MaskingKeySize) { EXPECT_EQ(kMaskingKeySize, 4); }

// ============================================================================
// Round-trip tests (build then parse)
// ============================================================================

TEST_F(WebSocketFrameTest, RoundTripUnmaskedText) {
  std::string_view original = "Hello, WebSocket!";
  BuildFrame(buffer, Opcode::Text, sv_bytes(original));

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, false);

  ASSERT_EQ(result.status, FrameParseResult::Status::Complete);
  std::string_view parsed(reinterpret_cast<const char *>(result.payload.data()), result.payload.size());
  EXPECT_EQ(parsed, original);
}

TEST_F(WebSocketFrameTest, RoundTripMaskedText) {
  std::string original = "Masked message";
  MaskingKey mask = {std::byte{0x37}, std::byte{0xFA}, std::byte{0x21}, std::byte{0x3D}};
  BuildFrame(buffer, Opcode::Text, sv_bytes(original), true, true, mask);

  // Make mutable copy for parsing (unmasking)
  std::vector<std::byte> mutableData(buffer.size());
  std::memcpy(mutableData.data(), buffer.data(), buffer.size());

  auto result = ParseFrame(mutableData, 0, true);

  ASSERT_EQ(result.status, FrameParseResult::Status::Complete);
  ASSERT_TRUE(result.header.masked);

  // Unmask the payload
  std::vector<std::byte> payloadCopy(result.payload.begin(), result.payload.end());
  ApplyMask(payloadCopy, result.header.maskingKey);

  std::string_view parsed(reinterpret_cast<const char *>(payloadCopy.data()), payloadCopy.size());
  EXPECT_EQ(parsed, original);
}

TEST_F(WebSocketFrameTest, RoundTripBinary) {
  std::array<uint8_t, 256> original;
  for (std::size_t idx = 0; idx < 256; ++idx) {
    original[idx] = static_cast<uint8_t>(idx);
  }

  BuildFrame(buffer, Opcode::Binary, container_bytes(original));

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, false);

  ASSERT_EQ(result.status, FrameParseResult::Status::Complete);
  EXPECT_EQ(result.header.opcode, Opcode::Binary);
  ASSERT_EQ(result.payload.size(), 256);

  for (std::size_t idx = 0; idx < 256; ++idx) {
    EXPECT_EQ(static_cast<uint8_t>(result.payload[idx]), original[idx]);
  }
}

// ============================================================================
// Additional coverage tests
// ============================================================================

TEST_F(WebSocketFrameTest, BuildCloseFrameNoStatusReceived) {
  // CloseCode::NoStatusReceived should produce empty payload
  BuildCloseFrame(buffer, CloseCode::NoStatusReceived, "ignored");

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, false);

  ASSERT_EQ(result.status, FrameParseResult::Status::Complete);
  EXPECT_EQ(result.header.opcode, Opcode::Close);
  EXPECT_TRUE(result.payload.empty());
}

TEST_F(WebSocketFrameTest, BuildCloseFrameReasonTruncated) {
  // Reason longer than 123 bytes (125 - 2 for code) should be truncated
  std::string longReason(200, 'X');
  BuildCloseFrame(buffer, CloseCode::Normal, longReason);

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, false);

  ASSERT_EQ(result.status, FrameParseResult::Status::Complete);
  // Payload should be 2 (code) + 123 (truncated reason) = 125
  EXPECT_EQ(result.payload.size(), kMaxControlFramePayload);
}

TEST_F(WebSocketFrameTest, BuildCloseFrameMasked) {
  MaskingKey mask = {std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF}, std::byte{0x12}};
  BuildCloseFrame(buffer, CloseCode::GoingAway, "bye", true, mask);

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, true);

  ASSERT_EQ(result.status, FrameParseResult::Status::Complete);
  EXPECT_TRUE(result.header.masked);

  // Unmask and verify
  std::vector<std::byte> payloadCopy(result.payload.begin(), result.payload.end());
  ApplyMask(payloadCopy, result.header.maskingKey);

  auto closePayload = ParseClosePayload(payloadCopy);
  EXPECT_EQ(closePayload.code, CloseCode::GoingAway);
  EXPECT_EQ(closePayload.reason, "bye");
}

TEST_F(WebSocketFrameTest, ParseClosePayloadSingleByte) {
  // Single byte payload is invalid
  std::array<std::byte, 1> payload = {std::byte{0x00}};
  auto closePayload = ParseClosePayload(payload);
  EXPECT_EQ(closePayload.code, CloseCode::ProtocolError);
}

TEST_F(WebSocketFrameTest, BuildMediumLengthFrame) {
  // Create a payload of 126 bytes to trigger 16-bit length encoding
  std::vector<std::byte> mediumPayload(126);
  for (std::size_t idx = 0; idx < mediumPayload.size(); ++idx) {
    mediumPayload[idx] = static_cast<std::byte>(idx & 0xFF);
  }

  BuildFrame(buffer, Opcode::Binary, mediumPayload);

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, false);

  ASSERT_EQ(result.status, FrameParseResult::Status::Complete);
  EXPECT_EQ(result.payload.size(), mediumPayload.size());
  // HeaderSize should be 4 (2 + 2 extended length)
  EXPECT_EQ(result.header.headerSize(), 4);
}

TEST_F(WebSocketFrameTest, Build64BitLengthFrame) {
  // Create a large payload that requires 64-bit length encoding
  std::vector<std::byte> largePayload(70000);
  for (std::size_t idx = 0; idx < largePayload.size(); ++idx) {
    largePayload[idx] = static_cast<std::byte>(idx & 0xFF);
  }

  BuildFrame(buffer, Opcode::Binary, largePayload);

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, false);

  ASSERT_EQ(result.status, FrameParseResult::Status::Complete);
  EXPECT_EQ(result.payload.size(), largePayload.size());
  EXPECT_EQ(result.header.headerSize(), 10);  // 2 + 8 extended length
}

TEST_F(WebSocketFrameTest, BuildMasked64BitLengthFrame) {
  std::vector<std::byte> largePayload(70000);
  MaskingKey mask = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};

  BuildFrame(buffer, Opcode::Binary, largePayload, true, true, mask);

  // Make mutable copy
  std::vector<std::byte> mutableData(buffer.size());
  std::memcpy(mutableData.data(), buffer.data(), buffer.size());

  auto result = ParseFrame(mutableData, 0, true);

  ASSERT_EQ(result.status, FrameParseResult::Status::Complete);
  EXPECT_TRUE(result.header.masked);
  EXPECT_EQ(result.payload.size(), 70000);
}

TEST_F(WebSocketFrameTest, ParseFrame64BitLengthNonMinimal) {
  // Build a frame with non-minimal 64-bit length encoding (value < 65536)
  std::vector<std::byte> frame;
  frame.push_back(std::byte{0x81});  // FIN + Text
  frame.push_back(std::byte{127});   // 64-bit length indicator
  // 8 bytes for length, value = 100 (could fit in 7-bit encoding)
  for (int idx = 0; idx < 7; ++idx) {
    frame.push_back(std::byte{0x00});
  }
  frame.push_back(std::byte{100});
  // 100 bytes of payload
  for (int idx = 0; idx < 100; ++idx) {
    frame.push_back(std::byte{'X'});
  }

  auto result = ParseFrame(frame, 0, false);
  EXPECT_EQ(result.status, FrameParseResult::Status::ProtocolError);
  EXPECT_TRUE(result.errorMessage.contains("minimal"));
}

TEST_F(WebSocketFrameTest, ParseFrame64BitLengthMSBSet) {
  // Build a frame with 64-bit length where MSB is set (invalid)
  std::vector<std::byte> frame;
  frame.push_back(std::byte{0x81});  // FIN + Text
  frame.push_back(std::byte{127});   // 64-bit length indicator
  frame.push_back(std::byte{0x80});  // MSB set
  for (int idx = 0; idx < 7; ++idx) {
    frame.push_back(std::byte{0x00});
  }

  auto result = ParseFrame(frame, 0, false);
  EXPECT_EQ(result.status, FrameParseResult::Status::ProtocolError);
  EXPECT_TRUE(result.errorMessage.contains("MSB"));
}

TEST_F(WebSocketFrameTest, BuildFrameNonFinFragment) {
  // Build a non-FIN frame (fragment)
  std::string_view payload = "fragment";
  BuildFrame(buffer, Opcode::Text, sv_bytes(payload), false);

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, false);

  ASSERT_EQ(result.status, FrameParseResult::Status::Complete);
  EXPECT_FALSE(result.header.fin);
}

TEST_F(WebSocketFrameTest, BuildEmptyPayloadMasked) {
  MaskingKey mask = {std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
  std::span<const std::byte> emptyPayload;
  BuildFrame(buffer, Opcode::Text, emptyPayload, true, true, mask);

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, true);

  ASSERT_EQ(result.status, FrameParseResult::Status::Complete);
  EXPECT_TRUE(result.header.masked);
  EXPECT_TRUE(result.payload.empty());
}

TEST_F(WebSocketFrameTest, IsReservedOpcodeValues) {
  // Test reserved opcodes
  EXPECT_TRUE(IsReservedOpcode(std::byte{3}));
  EXPECT_TRUE(IsReservedOpcode(std::byte{4}));
  EXPECT_TRUE(IsReservedOpcode(std::byte{5}));
  EXPECT_TRUE(IsReservedOpcode(std::byte{6}));
  EXPECT_TRUE(IsReservedOpcode(std::byte{7}));
  EXPECT_TRUE(IsReservedOpcode(std::byte{11}));
  EXPECT_TRUE(IsReservedOpcode(std::byte{12}));
  EXPECT_TRUE(IsReservedOpcode(std::byte{13}));
  EXPECT_TRUE(IsReservedOpcode(std::byte{14}));
  EXPECT_TRUE(IsReservedOpcode(std::byte{15}));

  // Non-reserved opcodes
  EXPECT_FALSE(IsReservedOpcode(std::byte{0}));
  EXPECT_FALSE(IsReservedOpcode(std::byte{1}));
  EXPECT_FALSE(IsReservedOpcode(std::byte{2}));
  EXPECT_FALSE(IsReservedOpcode(std::byte{8}));
  EXPECT_FALSE(IsReservedOpcode(std::byte{9}));
  EXPECT_FALSE(IsReservedOpcode(std::byte{10}));
  EXPECT_FALSE(IsReservedOpcode(std::byte{16}));
}

TEST_F(WebSocketFrameTest, IsControlFrameValues) {
  EXPECT_FALSE(IsControlFrame(Opcode::Continuation));
  EXPECT_FALSE(IsControlFrame(Opcode::Text));
  EXPECT_FALSE(IsControlFrame(Opcode::Binary));
  EXPECT_TRUE(IsControlFrame(Opcode::Close));
  EXPECT_TRUE(IsControlFrame(Opcode::Ping));
  EXPECT_TRUE(IsControlFrame(Opcode::Pong));
}

TEST_F(WebSocketFrameTest, IsDataFrameValues) {
  EXPECT_TRUE(IsDataFrame(Opcode::Continuation));
  EXPECT_TRUE(IsDataFrame(Opcode::Text));
  EXPECT_TRUE(IsDataFrame(Opcode::Binary));
  EXPECT_FALSE(IsDataFrame(Opcode::Close));
  EXPECT_FALSE(IsDataFrame(Opcode::Ping));
  EXPECT_FALSE(IsDataFrame(Opcode::Pong));
}

TEST_F(WebSocketFrameTest, IsValidWireCloseCode) {
  EXPECT_TRUE(IsValidWireCloseCode(static_cast<uint16_t>(CloseCode::InternalError)));
  EXPECT_TRUE(IsValidWireCloseCode(static_cast<uint16_t>(CloseCode::Normal)));
  EXPECT_TRUE(IsValidWireCloseCode(static_cast<uint16_t>(CloseCode::GoingAway)));
  EXPECT_TRUE(IsValidWireCloseCode(static_cast<uint16_t>(CloseCode::ProtocolError)));
  EXPECT_TRUE(IsValidWireCloseCode(static_cast<uint16_t>(CloseCode::UnsupportedData)));
  EXPECT_FALSE(IsValidWireCloseCode(static_cast<uint16_t>(CloseCode::NoStatusReceived)));
  EXPECT_FALSE(IsValidWireCloseCode(static_cast<uint16_t>(9999)));
  EXPECT_FALSE(IsValidWireCloseCode(static_cast<uint16_t>(999)));
  EXPECT_TRUE(IsValidWireCloseCode(static_cast<uint16_t>(3500)));
}

// ============================================================================
// RSV bits validation tests
// ============================================================================

TEST_F(WebSocketFrameTest, ParseRSV1BitSet) {
  // Build frame with RSV1 bit set
  std::vector<std::byte> frame;
  frame.push_back(std::byte{0x91});  // FIN=1, RSV1=1, opcode=Text
  frame.push_back(std::byte{0x05});  // MASK=0, length=5
  frame.push_back(std::byte{'H'});
  frame.push_back(std::byte{'e'});
  frame.push_back(std::byte{'l'});
  frame.push_back(std::byte{'l'});
  frame.push_back(std::byte{'o'});

  auto result = ParseFrame(frame, 0, false);
  EXPECT_EQ(result.status, FrameParseResult::Status::ProtocolError);
  EXPECT_TRUE(result.errorMessage.contains("Reserved"));
}

TEST_F(WebSocketFrameTest, ParseRSV2BitSet) {
  // Build frame with RSV2 bit set
  std::vector<std::byte> frame;
  frame.push_back(std::byte{0xA1});  // FIN=1, RSV2=1, opcode=Text
  frame.push_back(std::byte{0x00});  // MASK=0, length=0

  auto result = ParseFrame(frame, 0, false);
  EXPECT_EQ(result.status, FrameParseResult::Status::ProtocolError);
}

TEST_F(WebSocketFrameTest, ParseRSV3BitSet) {
  // Build frame with RSV3 bit set
  std::vector<std::byte> frame;
  frame.push_back(std::byte{0x91});  // FIN=1, RSV3=1, opcode=Text (0x81 | 0x10)
  frame.push_back(std::byte{0x00});  // MASK=0, length=0

  auto result = ParseFrame(frame, 0, false);
  EXPECT_EQ(result.status, FrameParseResult::Status::ProtocolError);
}

// ============================================================================
// Reserved opcode validation tests
// ============================================================================

TEST_F(WebSocketFrameTest, ParseReservedDataOpcode3) {
  std::vector<std::byte> frame;
  frame.push_back(std::byte{0x83});  // FIN=1, opcode=3 (reserved)
  frame.push_back(std::byte{0x00});  // MASK=0, length=0

  auto result = ParseFrame(frame, 0, false);
  EXPECT_EQ(result.status, FrameParseResult::Status::ProtocolError);
  EXPECT_TRUE(result.errorMessage.contains("Reserved opcode"));
}

TEST_F(WebSocketFrameTest, ParseReservedControlOpcode11) {
  std::vector<std::byte> frame;
  frame.push_back(std::byte{0x8B});  // FIN=1, opcode=0x0B (reserved control)
  frame.push_back(std::byte{0x00});  // MASK=0, length=0

  auto result = ParseFrame(frame, 0, false);
  EXPECT_EQ(result.status, FrameParseResult::Status::ProtocolError);
}

// ============================================================================
// Control frame fragmentation validation tests
// ============================================================================

TEST_F(WebSocketFrameTest, ParseFragmentedPingFrame) {
  std::vector<std::byte> frame;
  frame.push_back(std::byte{0x09});  // FIN=0, opcode=Ping (fragmented - invalid)
  frame.push_back(std::byte{0x00});  // MASK=0, length=0

  auto result = ParseFrame(frame, 0, false);
  EXPECT_EQ(result.status, FrameParseResult::Status::ProtocolError);
  EXPECT_TRUE(result.errorMessage.contains("fragmented"));
}

TEST_F(WebSocketFrameTest, ParseFragmentedCloseFrame) {
  std::vector<std::byte> frame;
  frame.push_back(std::byte{0x08});  // FIN=0, opcode=Close (fragmented - invalid)
  frame.push_back(std::byte{0x00});  // MASK=0, length=0

  auto result = ParseFrame(frame, 0, false);
  EXPECT_EQ(result.status, FrameParseResult::Status::ProtocolError);
}

// ============================================================================
// Control frame payload too large tests
// ============================================================================

TEST_F(WebSocketFrameTest, ParsePingPayloadTooLarge) {
  // Build Ping frame with payload > 125 bytes (using 16-bit length)
  std::vector<std::byte> frame;
  frame.push_back(std::byte{0x89});  // FIN=1, opcode=Ping
  frame.push_back(std::byte{126});   // 16-bit length indicator
  frame.push_back(std::byte{0x00});
  frame.push_back(std::byte{130});  // 130 bytes > 125
  for (int idx = 0; idx < 130; ++idx) {
    frame.push_back(std::byte{'X'});
  }

  auto result = ParseFrame(frame, 0, false);
  EXPECT_EQ(result.status, FrameParseResult::Status::ProtocolError);
  EXPECT_TRUE(result.errorMessage.contains("payload too large"));
}

// ============================================================================
// Server frames must be masked validation
// ============================================================================

TEST_F(WebSocketFrameTest, ParseClientRejectsServerMaskedFrame) {
  // Client receiving masked frame from server (invalid)
  std::string_view payload = "test";
  MaskingKey mask = {std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
  BuildFrame(buffer, Opcode::Text, sv_bytes(payload), true, true, mask);

  auto data = buf_bytes(buffer);
  auto result = ParseFrame(data, 0, false);  // Client-side

  EXPECT_EQ(result.status, FrameParseResult::Status::ProtocolError);
  EXPECT_TRUE(result.errorMessage.contains("must not be masked"));
}

// ============================================================================
// 16-bit length non-minimal encoding tests
// ============================================================================

TEST_F(WebSocketFrameTest, ParseFrame16BitLengthNonMinimal) {
  // Build a frame with non-minimal 16-bit length encoding (value < 126)
  std::vector<std::byte> frame;
  frame.push_back(std::byte{0x81});  // FIN + Text
  frame.push_back(std::byte{126});   // 16-bit length indicator
  // 2 bytes for length, value = 50 (could fit in 7-bit encoding)
  frame.push_back(std::byte{0x00});
  frame.push_back(std::byte{50});
  // 50 bytes of payload
  for (int idx = 0; idx < 50; ++idx) {
    frame.push_back(std::byte{'X'});
  }

  auto result = ParseFrame(frame, 0, false);
  EXPECT_EQ(result.status, FrameParseResult::Status::ProtocolError);
  EXPECT_TRUE(result.errorMessage.contains("minimal"));
}

// ============================================================================
// Incomplete masking key tests
// ============================================================================

TEST_F(WebSocketFrameTest, ParseIncompleteMaskingKey) {
  // Header says masked, but not enough bytes for masking key
  std::vector<std::byte> frame;
  frame.push_back(std::byte{0x81});  // FIN + Text
  frame.push_back(std::byte{0x85});  // MASK=1, length=5
  frame.push_back(std::byte{0x12});  // Only 1 byte of masking key

  auto result = ParseFrame(frame, 0, true);  // Server-side
  EXPECT_EQ(result.status, FrameParseResult::Status::Incomplete);
}

// ============================================================================
// Incomplete 64-bit extended length tests
// ============================================================================

TEST_F(WebSocketFrameTest, ParseIncomplete64BitLength) {
  // 64-bit length marker but only 4 extra bytes
  std::vector<std::byte> frame;
  frame.push_back(std::byte{0x81});  // FIN + Text
  frame.push_back(std::byte{127});   // 64-bit length indicator
  frame.push_back(std::byte{0x00});
  frame.push_back(std::byte{0x00});
  frame.push_back(std::byte{0x00});
  frame.push_back(std::byte{0x00});  // Only 4 bytes, need 8

  auto result = ParseFrame(frame, 0, false);
  EXPECT_EQ(result.status, FrameParseResult::Status::Incomplete);
}

// ============================================================================
// ApplyMask small data path tests
// ============================================================================

TEST_F(WebSocketFrameTest, ApplyMaskSmallData) {
  // Test with data < 8 bytes (uses byte-by-byte path)
  std::array<std::byte, 5> data = {std::byte{'H'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'}};
  MaskingKey mask = {std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};

  std::array<std::byte, 5> expected;
  for (std::size_t idx = 0; idx < 5; ++idx) {
    expected[idx] =
        std::byte{static_cast<uint8_t>(static_cast<uint8_t>(data[idx]) ^ static_cast<uint8_t>(mask[idx % 4]))};
  }

  ApplyMask(data, mask);

  for (std::size_t idx = 0; idx < 5; ++idx) {
    EXPECT_EQ(data[idx], expected[idx]);
  }
}

TEST_F(WebSocketFrameTest, ApplyMaskExactly8Bytes) {
  // Test with exactly 8 bytes (boundary condition for 64-bit optimization)
  std::array<std::byte, 8> data;
  std::ranges::fill(data, std::byte{0xAB});
  MaskingKey mask = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};

  std::array<std::byte, 8> original = data;
  ApplyMask(data, mask);

  // Verify mask was applied
  EXPECT_NE(std::memcmp(data.data(), original.data(), 8), 0);

  // Unmask and verify original restored
  ApplyMask(data, mask);
  EXPECT_EQ(std::memcmp(data.data(), original.data(), 8), 0);
}

// ============================================================================
// Header size edge case tests
// ============================================================================

TEST_F(WebSocketFrameTest, HeaderSizeBoundary126) {
  FrameHeader header{};
  header.masked = false;
  header.payloadLength = 126;         // Exactly at 16-bit threshold
  EXPECT_EQ(header.headerSize(), 4);  // 2 + 2 (extended length)
}

TEST_F(WebSocketFrameTest, HeaderSizeBoundary65535) {
  FrameHeader header{};
  header.masked = false;
  header.payloadLength = 65535;       // Exactly at 16-bit max
  EXPECT_EQ(header.headerSize(), 4);  // Still 16-bit encoding
}

TEST_F(WebSocketFrameTest, HeaderSizeBoundary65536) {
  FrameHeader header{};
  header.masked = false;
  header.payloadLength = 65536;        // First value needing 64-bit
  EXPECT_EQ(header.headerSize(), 10);  // 2 + 8 (extended length)
}

}  // namespace aeronet::websocket
