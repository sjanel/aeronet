#include "aeronet/upgrade-handler.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/concatenated-strings.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-deflate.hpp"

namespace aeronet {
namespace {

// Valid WebSocket key (24 base64 chars representing 16 bytes)
constexpr std::string_view kValidWebSocketKey = "dGhlIHNhbXBsZSBub25jZQ==";

// Expected Sec-WebSocket-Accept for the above key (computed per RFC 6455)
constexpr std::string_view kExpectedWebSocketAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

// Helper to build a raw HTTP request
RawChars BuildRaw(std::string_view method, std::string_view target, std::string_view extraHeaders = {}) {
  RawChars raw;
  raw.append(method);
  raw.push_back(' ');
  raw.append(target);
  raw.append(" HTTP/1.1\r\n");
  raw.append("Host: example\r\n");
  raw.append(extraHeaders);
  raw.append(http::CRLF);
  return raw;
}

}  // namespace

// Test harness for parsing HTTP requests (outside anonymous namespace for friend access)
class UpgradeHandlerHarness : public ::testing::Test {
 protected:
  http::StatusCode parse(RawChars raw) {
    connState.inBuffer = std::move(raw);
    RawChars tmp;
    return request.initTrySetHead(connState, tmp, 4096U, true, nullptr);
  }

  HttpRequest request;
  ConnectionState connState;
};

namespace {

// Tests for ConnectionContainsUpgrade
TEST(UpgradeHandlerTest, ConnectionContainsUpgrade_Simple) {
  EXPECT_TRUE(upgrade::ConnectionContainsUpgrade("upgrade"));
  EXPECT_TRUE(upgrade::ConnectionContainsUpgrade("Upgrade"));
  EXPECT_TRUE(upgrade::ConnectionContainsUpgrade("UPGRADE"));
}

TEST(UpgradeHandlerTest, ConnectionContainsUpgrade_WithOtherTokens) {
  EXPECT_TRUE(upgrade::ConnectionContainsUpgrade("keep-alive, upgrade"));
  EXPECT_TRUE(upgrade::ConnectionContainsUpgrade("keep-alive, Upgrade, close"));
  EXPECT_TRUE(upgrade::ConnectionContainsUpgrade("Upgrade, keep-alive"));
}

TEST(UpgradeHandlerTest, ConnectionContainsUpgrade_WithWhitespace) {
  EXPECT_TRUE(upgrade::ConnectionContainsUpgrade(" upgrade "));
  EXPECT_TRUE(upgrade::ConnectionContainsUpgrade("keep-alive , upgrade , close"));
}

TEST(UpgradeHandlerTest, ConnectionContainsUpgrade_NoUpgrade) {
  EXPECT_FALSE(upgrade::ConnectionContainsUpgrade("keep-alive"));
  EXPECT_FALSE(upgrade::ConnectionContainsUpgrade("close"));
  EXPECT_FALSE(upgrade::ConnectionContainsUpgrade(""));
}

// Tests for IsValidWebSocketKey
TEST(UpgradeHandlerTest, IsValidWebSocketKey_ValidKey) {
  // Valid key: exactly 24 base64 characters
  EXPECT_TRUE(upgrade::IsValidWebSocketKey(kValidWebSocketKey));
}

TEST(UpgradeHandlerTest, IsValidWebSocketKey_TooShort) {
  EXPECT_FALSE(upgrade::IsValidWebSocketKey("dGhlIHNhbXBsZSBub25jZQ="));
}

TEST(UpgradeHandlerTest, IsValidWebSocketKey_TooLong) {
  EXPECT_FALSE(upgrade::IsValidWebSocketKey("dGhlIHNhbXBsZSBub25jZQ==="));
}

TEST(UpgradeHandlerTest, IsValidWebSocketKey_InvalidCharacters) {
  EXPECT_FALSE(upgrade::IsValidWebSocketKey("dGhlIHNhbXBsZSBub25j@Q=="));
}

TEST(UpgradeHandlerTest, IsValidWebSocketKey_Empty) { EXPECT_FALSE(upgrade::IsValidWebSocketKey("")); }

TEST(UpgradeHandlerTest, IsValidWebSocketKey_NotEndingWithDoubleEquals) {
  EXPECT_FALSE(upgrade::IsValidWebSocketKey("dGhlIHNhbXBsZSBub25jZQAA"));
}

// Test for ComputeWebSocketAccept
TEST(UpgradeHandlerTest, ComputeWebSocketAccept_RFC6455TestVector) {
  // RFC 6455 test vector
  const auto accept = upgrade::ComputeWebSocketAccept(kValidWebSocketKey);

  EXPECT_EQ(std::string_view(accept.data(), accept.size()), kExpectedWebSocketAccept);
}

// ============================================================================
// ValidateWebSocketUpgrade tests using real HttpRequest parsing
// ============================================================================
TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_ValidRequest) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_TRUE(result.valid);
  EXPECT_EQ(result.targetProtocol, ProtocolType::WebSocket);
  EXPECT_TRUE(result.errorMessage.empty());
  EXPECT_EQ(std::string_view(result.secWebSocketAccept.data(), result.secWebSocketAccept.size()),
            kExpectedWebSocketAccept);
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_MissingUpgradeHeader) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(result.errorMessage.contains("Upgrade"));
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_WrongUpgradeValue) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: http2\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(result.errorMessage.contains("websocket"));
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_MissingConnectionHeader) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(result.errorMessage.contains("Connection"));
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_MissingVersion) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(result.errorMessage.contains("Version"));
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_WrongVersion) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 8\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(result.errorMessage.contains("13"));
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_MissingKey) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);
  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(result.errorMessage.contains("Key"));
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_InvalidKeyFormat) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: tooshort\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(result.errorMessage.contains("Key"));
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_WithProtocol) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"
                                     "Sec-WebSocket-Protocol: graphql-ws, chat\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_TRUE(result.valid);

  // Check offered protocols are captured
  ASSERT_EQ(result.offeredProtocols.nbConcatenatedStrings(), 2);
  EXPECT_TRUE(result.offeredProtocols.contains("graphql-ws"));
  EXPECT_TRUE(result.offeredProtocols.contains("chat"));
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_SubprotocolNegotiation) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"
                                     "Sec-WebSocket-Protocol: graphql-ws, chat, json\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  // Server supports "json" and "chat", prefers "json"
  ConcatenatedStrings serverProtocols;
  serverProtocols.append("json");
  serverProtocols.append("chat");
  WebSocketUpgradeConfig config{serverProtocols, false, {}};

  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_TRUE(result.valid);

  // Should select "json" (server's first preference that client offers)
  EXPECT_EQ(result.selectedProtocol, "json");
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_SubprotocolNoMatch) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"
                                     "Sec-WebSocket-Protocol: graphql-ws, chat\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  // Server supports "binary" and "xml" - no match with client
  ConcatenatedStrings serverProtocols;
  serverProtocols.append("binary");
  serverProtocols.append("xml");
  WebSocketUpgradeConfig config{serverProtocols, false, {}};

  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_TRUE(result.valid);  // Still valid, just no protocol selected
  EXPECT_TRUE(result.selectedProtocol.empty());
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_SubprotocolCaseInsensitive) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"
                                     "Sec-WebSocket-Protocol: GraphQL-WS\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  serverProtocols.append("graphql-ws");
  WebSocketUpgradeConfig config{serverProtocols, false, {}};

  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_TRUE(result.valid);
  EXPECT_EQ(result.selectedProtocol, "graphql-ws");
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_WithExtensions) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"
                                     "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_TRUE(result.valid);

  // Extensions are captured for informational purposes
  ASSERT_EQ(result.offeredExtensions.nbConcatenatedStrings(), 1);
  EXPECT_TRUE(result.offeredExtensions.begin()->starts_with("permessage-deflate"));
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_PermessageDeflateNegotiation) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"
                                     "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  WebSocketUpgradeConfig config{{}, true, {}};

  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);

#ifdef AERONET_ENABLE_ZLIB
  // Compression should be negotiated
  EXPECT_TRUE(result.valid);
  ASSERT_TRUE(result.deflateParams.has_value());
  EXPECT_EQ(result.deflateParams.value_or(websocket::DeflateNegotiatedParams{}).serverMaxWindowBits, 15);
  EXPECT_EQ(result.deflateParams.value_or(websocket::DeflateNegotiatedParams{}).clientMaxWindowBits, 15);
  EXPECT_FALSE(result.deflateParams.value_or(websocket::DeflateNegotiatedParams{}).serverNoContextTakeover);
  EXPECT_FALSE(result.deflateParams.value_or(websocket::DeflateNegotiatedParams{}).clientNoContextTakeover);
#else
  // Compression not supported in this build
  EXPECT_FALSE(result.valid);
  EXPECT_FALSE(result.deflateParams.has_value());
#endif
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_PermessageDeflateWithParams) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"
                                     "Sec-WebSocket-Extensions: permessage-deflate; server_max_window_bits=10; "
                                     "client_no_context_takeover\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  WebSocketUpgradeConfig config{{}, true, {}};

  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);

#ifdef AERONET_ENABLE_ZLIB
  EXPECT_TRUE(result.valid);
  ASSERT_TRUE(result.deflateParams.has_value());
  EXPECT_EQ(result.deflateParams.value_or(websocket::DeflateNegotiatedParams{}).serverMaxWindowBits, 10);
  EXPECT_TRUE(result.deflateParams.value_or(websocket::DeflateNegotiatedParams{}).clientNoContextTakeover);
#else
  EXPECT_FALSE(result.valid);
  EXPECT_FALSE(result.deflateParams.has_value());
#endif
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_PermessageDeflateDisabled) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"
                                     "Sec-WebSocket-Extensions: permessage-deflate\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};

  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_TRUE(result.valid);

  // Compression should NOT be negotiated
  EXPECT_FALSE(result.deflateParams.has_value());
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_ConnectionWithMultipleTokens) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: keep-alive, Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_TRUE(result.valid);
}

// ============================================================================
// ValidateHttp2Upgrade tests
// ============================================================================

TEST_F(UpgradeHandlerHarness, ValidateHttp2Upgrade_ValidRequest) {
  const auto status = parse(BuildRaw("GET", "/resource",
                                     "Upgrade: h2c\r\n"
                                     "Connection: Upgrade, HTTP2-Settings\r\n"
                                     "HTTP2-Settings: AAMAAABkAARAAAAAAAIAAAAA\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = upgrade::ValidateHttp2Upgrade(request);
  EXPECT_TRUE(result.valid);
  EXPECT_EQ(result.targetProtocol, ProtocolType::Http2);
}

TEST_F(UpgradeHandlerHarness, ValidateHttp2Upgrade_MissingUpgradeHeader) {
  const auto status = parse(BuildRaw("GET", "/resource",
                                     "Connection: Upgrade, HTTP2-Settings\r\n"
                                     "HTTP2-Settings: AAMAAABkAARAAAAAAAIAAAAA\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = upgrade::ValidateHttp2Upgrade(request);
  EXPECT_FALSE(result.valid);
}

TEST_F(UpgradeHandlerHarness, ValidateHttp2Upgrade_WrongUpgradeValue) {
  const auto status = parse(BuildRaw("GET", "/resource",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade, HTTP2-Settings\r\n"
                                     "HTTP2-Settings: AAMAAABkAARAAAAAAAIAAAAA\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = upgrade::ValidateHttp2Upgrade(request);
  EXPECT_FALSE(result.valid);
}

TEST_F(UpgradeHandlerHarness, ValidateHttp2Upgrade_MissingConnectionHeader) {
  const auto status = parse(BuildRaw("GET", "/resource",
                                     "Upgrade: h2c\r\n"
                                     "HTTP2-Settings: AAMAAABkAARAAAAAAAIAAAAA\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = upgrade::ValidateHttp2Upgrade(request);
  EXPECT_FALSE(result.valid);
}

TEST_F(UpgradeHandlerHarness, ValidateHttp2Upgrade_ConnectionWithoutUpgrade) {
  const auto status = parse(BuildRaw("GET", "/resource",
                                     "Upgrade: h2c\r\n"
                                     "Connection: keep-alive\r\n"
                                     "HTTP2-Settings: AAMAAABkAARAAAAAAAIAAAAA\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = upgrade::ValidateHttp2Upgrade(request);
  EXPECT_FALSE(result.valid);
}

TEST_F(UpgradeHandlerHarness, ValidateHttp2Upgrade_MissingSettings) {
  const auto status = parse(BuildRaw("GET", "/resource",
                                     "Upgrade: h2c\r\n"
                                     "Connection: Upgrade, HTTP2-Settings\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = upgrade::ValidateHttp2Upgrade(request);
  EXPECT_FALSE(result.valid);
}

TEST_F(UpgradeHandlerHarness, ValidateHttp2Upgrade_EmptySettings) {
  const auto status = parse(BuildRaw("GET", "/resource",
                                     "Upgrade: h2c\r\n"
                                     "Connection: Upgrade, HTTP2-Settings\r\n"
                                     "HTTP2-Settings: \r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = upgrade::ValidateHttp2Upgrade(request);
  EXPECT_FALSE(result.valid);
}

// ============================================================================
// DetectUpgradeTarget tests
// ============================================================================

TEST_F(UpgradeHandlerHarness, DetectUpgradeTarget_WebSocket) {
  const auto status = parse(BuildRaw("GET", "/ws", "Upgrade: websocket\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  EXPECT_EQ(upgrade::DetectUpgradeTarget(request), ProtocolType::WebSocket);
}

TEST_F(UpgradeHandlerHarness, DetectUpgradeTarget_WebSocketCaseInsensitive) {
  const auto status = parse(BuildRaw("GET", "/ws", "Upgrade: WEBSOCKET\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  EXPECT_EQ(upgrade::DetectUpgradeTarget(request), ProtocolType::WebSocket);
}

TEST_F(UpgradeHandlerHarness, DetectUpgradeTarget_Http2) {
  const auto status = parse(BuildRaw("GET", "/", "Upgrade: h2c\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  EXPECT_EQ(upgrade::DetectUpgradeTarget(request), ProtocolType::Http2);
}

TEST_F(UpgradeHandlerHarness, DetectUpgradeTarget_Http2CaseInsensitive) {
  const auto status = parse(BuildRaw("GET", "/", "Upgrade: H2C\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  EXPECT_EQ(upgrade::DetectUpgradeTarget(request), ProtocolType::Http2);
}

TEST_F(UpgradeHandlerHarness, DetectUpgradeTarget_NoUpgrade) {
  const auto status = parse(BuildRaw("GET", "/"));
  ASSERT_EQ(status, http::StatusCodeOK);

  EXPECT_EQ(upgrade::DetectUpgradeTarget(request), ProtocolType::Http11);
}

TEST_F(UpgradeHandlerHarness, DetectUpgradeTarget_UnknownProtocol) {
  const auto status = parse(BuildRaw("GET", "/", "Upgrade: unknown-protocol\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  EXPECT_EQ(upgrade::DetectUpgradeTarget(request), ProtocolType::Http11);
}

TEST_F(UpgradeHandlerHarness, DetectUpgradeTarget_WithWhitespace) {
  const auto status = parse(BuildRaw("GET", "/ws", "Upgrade:  websocket \r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  EXPECT_EQ(upgrade::DetectUpgradeTarget(request), ProtocolType::WebSocket);
}

// Tests for BuildWebSocketUpgradeResponse
TEST(UpgradeHandlerTest, BuildWebSocketUpgradeResponse_Basic) {
  UpgradeValidationResult validationResult;
  validationResult.valid = true;
  validationResult.targetProtocol = ProtocolType::WebSocket;
  std::copy_n(kExpectedWebSocketAccept.data(), kExpectedWebSocketAccept.size(),
              validationResult.secWebSocketAccept.data());

  const auto response = upgrade::BuildWebSocketUpgradeResponse(validationResult);
  const std::string_view responseView(response.data(), response.size());

  // Check status line
  EXPECT_TRUE(responseView.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));

  // Check required headers are present
  EXPECT_TRUE(responseView.contains("Upgrade: websocket\r\n"));
  EXPECT_TRUE(responseView.contains("Connection: Upgrade\r\n"));
  EXPECT_TRUE(responseView.contains("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"));

  // Check response ends with double CRLF
  EXPECT_TRUE(responseView.ends_with("\r\n\r\n"));
}

TEST(UpgradeHandlerTest, BuildWebSocketUpgradeResponse_WithProtocol) {
  UpgradeValidationResult validationResult;
  validationResult.valid = true;
  validationResult.targetProtocol = ProtocolType::WebSocket;
  std::copy_n(kExpectedWebSocketAccept.data(), kExpectedWebSocketAccept.size(),
              validationResult.secWebSocketAccept.data());
  validationResult.selectedProtocol = "graphql-ws";

  const auto response = upgrade::BuildWebSocketUpgradeResponse(validationResult);
  const std::string_view responseView(response.data(), response.size());

  EXPECT_TRUE(responseView.contains("Sec-WebSocket-Protocol: graphql-ws\r\n"));
}

TEST(UpgradeHandlerTest, BuildWebSocketUpgradeResponse_WithDeflate) {
  UpgradeValidationResult validationResult;
  validationResult.valid = true;
  validationResult.targetProtocol = ProtocolType::WebSocket;
  std::copy_n(kExpectedWebSocketAccept.data(), kExpectedWebSocketAccept.size(),
              validationResult.secWebSocketAccept.data());
  validationResult.deflateParams = websocket::DeflateNegotiatedParams{.serverMaxWindowBits = 12,
                                                                      .clientMaxWindowBits = 15,
                                                                      .serverNoContextTakeover = true,
                                                                      .clientNoContextTakeover = false};

  const auto response = upgrade::BuildWebSocketUpgradeResponse(validationResult);
  const std::string_view responseView(response.data(), response.size());

  // Check extension header is present
  EXPECT_TRUE(responseView.contains("Sec-WebSocket-Extensions: permessage-deflate"));
  EXPECT_TRUE(responseView.contains("server_no_context_takeover"));
  EXPECT_TRUE(responseView.contains("server_max_window_bits=12"));
  // client_max_window_bits=15 is default, should not appear
  EXPECT_FALSE(responseView.contains("client_max_window_bits"));
}

// Tests for BuildHttp2UpgradeResponse
TEST(UpgradeHandlerTest, BuildHttp2UpgradeResponse_Basic) {
  UpgradeValidationResult validationResult;
  validationResult.valid = true;
  validationResult.targetProtocol = ProtocolType::Http2;

  const auto response = upgrade::BuildHttp2UpgradeResponse(validationResult);
  const std::string_view responseView(response.data(), response.size());

  // Check status line
  EXPECT_TRUE(responseView.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));

  // Check required headers are present
  EXPECT_TRUE(responseView.contains("Upgrade: h2c\r\n"));
  EXPECT_TRUE(responseView.contains("Connection: Upgrade\r\n"));

  // Check response ends with double CRLF
  EXPECT_TRUE(responseView.ends_with("\r\n\r\n"));
}

// WebSocket constants tests
TEST(WebSocketConstantsTest, OpcodeValues) {
  EXPECT_EQ(static_cast<uint8_t>(websocket::Opcode::Continuation), 0x00);
  EXPECT_EQ(static_cast<uint8_t>(websocket::Opcode::Text), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(websocket::Opcode::Binary), 0x02);
  EXPECT_EQ(static_cast<uint8_t>(websocket::Opcode::Close), 0x08);
  EXPECT_EQ(static_cast<uint8_t>(websocket::Opcode::Ping), 0x09);
  EXPECT_EQ(static_cast<uint8_t>(websocket::Opcode::Pong), 0x0A);
}

TEST(WebSocketConstantsTest, CloseCodeValues) {
  EXPECT_EQ(static_cast<uint16_t>(websocket::CloseCode::Normal), 1000);
  EXPECT_EQ(static_cast<uint16_t>(websocket::CloseCode::GoingAway), 1001);
  EXPECT_EQ(static_cast<uint16_t>(websocket::CloseCode::ProtocolError), 1002);
  EXPECT_EQ(static_cast<uint16_t>(websocket::CloseCode::UnsupportedData), 1003);
}

TEST(WebSocketConstantsTest, HeaderNames) {
  EXPECT_EQ(websocket::SecWebSocketKey, "Sec-WebSocket-Key");
  EXPECT_EQ(websocket::SecWebSocketAccept, "Sec-WebSocket-Accept");
  EXPECT_EQ(websocket::SecWebSocketVersion, "Sec-WebSocket-Version");
  EXPECT_EQ(websocket::SecWebSocketProtocol, "Sec-WebSocket-Protocol");
  EXPECT_EQ(websocket::SecWebSocketExtensions, "Sec-WebSocket-Extensions");
  EXPECT_EQ(websocket::UpgradeValue, "websocket");
}

// HTTP/2 constants tests
TEST(Http2ConstantsTest, FrameTypeValues) {
  EXPECT_EQ(static_cast<uint8_t>(http2::FrameType::Data), 0x00);
  EXPECT_EQ(static_cast<uint8_t>(http2::FrameType::Headers), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(http2::FrameType::Settings), 0x04);
  EXPECT_EQ(static_cast<uint8_t>(http2::FrameType::Ping), 0x06);
  EXPECT_EQ(static_cast<uint8_t>(http2::FrameType::GoAway), 0x07);
  EXPECT_EQ(static_cast<uint8_t>(http2::FrameType::WindowUpdate), 0x08);
}

TEST(Http2ConstantsTest, ErrorCodeValues) {
  EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::NoError), 0x00);
  EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::ProtocolError), 0x01);
  EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::FlowControlError), 0x03);
}

TEST(Http2ConstantsTest, SettingsParameterValues) {
  EXPECT_EQ(static_cast<uint16_t>(http2::SettingsParameter::HeaderTableSize), 0x01);
  EXPECT_EQ(static_cast<uint16_t>(http2::SettingsParameter::EnablePush), 0x02);
  EXPECT_EQ(static_cast<uint16_t>(http2::SettingsParameter::MaxConcurrentStreams), 0x03);
  EXPECT_EQ(static_cast<uint16_t>(http2::SettingsParameter::InitialWindowSize), 0x04);
  EXPECT_EQ(static_cast<uint16_t>(http2::SettingsParameter::MaxFrameSize), 0x05);
  EXPECT_EQ(static_cast<uint16_t>(http2::SettingsParameter::MaxHeaderListSize), 0x06);
}

TEST(Http2ConstantsTest, ConnectionPreface) {
  EXPECT_EQ(http2::kConnectionPreface, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
  EXPECT_EQ(http2::kConnectionPrefaceSize, 24);
}

TEST(Http2ConstantsTest, AlpnIdentifiers) {
  EXPECT_EQ(http2::kAlpnH2, "h2");
  EXPECT_EQ(http2::kAlpnH2c, "h2c");
}

// Protocol type tests
TEST(ProtocolTypeTest, Values) {
  EXPECT_EQ(static_cast<uint8_t>(ProtocolType::Http11), 0);
  EXPECT_EQ(static_cast<uint8_t>(ProtocolType::WebSocket), 1);
  EXPECT_EQ(static_cast<uint8_t>(ProtocolType::Http2), 2);
}

// ============================================================================
// Additional ConnectionContainsUpgrade edge case tests
// ============================================================================

TEST(UpgradeHandlerTest, ConnectionContainsUpgrade_EmptyToken) {
  EXPECT_FALSE(upgrade::ConnectionContainsUpgrade(","));
  EXPECT_FALSE(upgrade::ConnectionContainsUpgrade(",,"));
  EXPECT_TRUE(upgrade::ConnectionContainsUpgrade(",upgrade,"));
}

TEST(UpgradeHandlerTest, ConnectionContainsUpgrade_SingleUpgrade) {
  EXPECT_TRUE(upgrade::ConnectionContainsUpgrade("upgrade"));
}

TEST(UpgradeHandlerTest, ConnectionContainsUpgrade_TrailingComma) {
  EXPECT_TRUE(upgrade::ConnectionContainsUpgrade("upgrade,"));
  EXPECT_TRUE(upgrade::ConnectionContainsUpgrade("keep-alive,upgrade,"));
}

// ============================================================================
// IsValidWebSocketKey edge case tests
// ============================================================================

TEST(UpgradeHandlerTest, IsValidWebSocketKey_ExactlyWrongPadding) {
  // Key with wrong padding position
  EXPECT_FALSE(upgrade::IsValidWebSocketKey("dGhlIHNhbXBsZSBub25jZQA="));  // Single = at wrong position
}

TEST(UpgradeHandlerTest, IsValidWebSocketKey_NonBase64InMiddle) {
  EXPECT_FALSE(upgrade::IsValidWebSocketKey("dGhlIHNhbXBsZS!ub25jZQ=="));  // ! is not base64
}

TEST(UpgradeHandlerTest, IsValidWebSocketKey_SpacesInKey) {
  EXPECT_FALSE(upgrade::IsValidWebSocketKey("dGhlIHNhbXBsZS ub25jZQ=="));  // Space not allowed
}

// ============================================================================
// Additional ValidateWebSocketUpgrade tests
// ============================================================================

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_UpgradeHeaderWithWhitespace) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade:  websocket  \r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_TRUE(result.valid);
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_ConnectionUpgradeWithExtraTokens) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: keep-alive, Upgrade, HTTP2-Settings\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_TRUE(result.valid);
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_VersionWithWhitespace) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version:  13  \r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_TRUE(result.valid);
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_KeyWithWhitespace) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key:  dGhlIHNhbXBsZSBub25jZQ==  \r\n"
                                     "Sec-WebSocket-Version: 13\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_TRUE(result.valid);
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_ConnectionNoUpgradeToken) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: keep-alive, close\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(result.errorMessage.contains("upgrade"));
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_MultipleExtensions) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"
                                     "Sec-WebSocket-Extensions: x-webkit-deflate-frame, permessage-deflate\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  WebSocketUpgradeConfig config{{}, true, {}};

  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);

// Should pick the first acceptable extension (permessage-deflate)
// x-webkit-deflate-frame is not supported
#ifdef AERONET_ENABLE_ZLIB
  EXPECT_TRUE(result.valid);
  EXPECT_TRUE(result.deflateParams.has_value());
#else
  EXPECT_FALSE(result.valid);
  EXPECT_FALSE(result.deflateParams.has_value());
#endif
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_EmptyProtocolHeader) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"
                                     "Sec-WebSocket-Protocol: \r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};
  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_TRUE(result.valid);
  EXPECT_TRUE(result.offeredProtocols.empty());
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_EmptyExtensionsHeader) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"
                                     "Sec-WebSocket-Extensions: \r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  WebSocketUpgradeConfig config{{}, true, {}};

  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_TRUE(result.valid);
  EXPECT_FALSE(result.deflateParams.has_value());
}

TEST_F(UpgradeHandlerHarness, ValidateWebSocketUpgrade_NoSupportedProtocols) {
  const auto status = parse(BuildRaw("GET", "/ws",
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n"
                                     "Sec-WebSocket-Protocol: graphql-ws, chat\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  ConcatenatedStrings serverProtocols;
  WebSocketUpgradeConfig config{serverProtocols, false, {}};

  const auto result = upgrade::ValidateWebSocketUpgrade(request, config);
  EXPECT_TRUE(result.valid);
  // Protocols are captured but none selected
  EXPECT_EQ(result.offeredProtocols.nbConcatenatedStrings(), 2);
  EXPECT_TRUE(result.selectedProtocol.empty());
}

// ============================================================================
// BuildWebSocketUpgradeResponse additional tests
// ============================================================================

TEST(UpgradeHandlerTest, BuildWebSocketUpgradeResponse_NoProtocolNoDeflate) {
  UpgradeValidationResult validationResult;
  validationResult.valid = true;
  validationResult.targetProtocol = ProtocolType::WebSocket;
  std::copy_n("testaccept", std::min<size_t>(std::strlen("testaccept"), validationResult.secWebSocketAccept.size()),
              validationResult.secWebSocketAccept.data());

  const auto response = upgrade::BuildWebSocketUpgradeResponse(validationResult);
  const std::string_view responseView(response);

  // Should not contain protocol or extensions headers
  EXPECT_FALSE(responseView.contains("Sec-WebSocket-Protocol"));
  EXPECT_FALSE(responseView.contains("Sec-WebSocket-Extensions"));
}

TEST(UpgradeHandlerTest, BuildWebSocketUpgradeResponse_WithDeflateNoContextTakeover) {
  UpgradeValidationResult validationResult;
  validationResult.valid = true;
  validationResult.targetProtocol = ProtocolType::WebSocket;
  std::copy_n("testaccept", std::min<size_t>(std::strlen("testaccept"), validationResult.secWebSocketAccept.size()),
              validationResult.secWebSocketAccept.data());
  validationResult.deflateParams = websocket::DeflateNegotiatedParams{.serverMaxWindowBits = 15,
                                                                      .clientMaxWindowBits = 15,
                                                                      .serverNoContextTakeover = true,
                                                                      .clientNoContextTakeover = true};

  const auto response = upgrade::BuildWebSocketUpgradeResponse(validationResult);
  const std::string_view responseView(response);

  EXPECT_TRUE(responseView.contains("server_no_context_takeover"));
  EXPECT_TRUE(responseView.contains("client_no_context_takeover"));
}

// ============================================================================
// ComputeWebSocketAccept edge cases
// ============================================================================

TEST(UpgradeHandlerTest, ComputeWebSocketAccept_EmptyKey) {
  const auto accept = upgrade::ComputeWebSocketAccept("");
  // Should still work - concatenates empty string with GUID
  EXPECT_NE(accept[0], '\0');
}

TEST(UpgradeHandlerTest, ComputeWebSocketAccept_LongKey) {
  std::string longKey(1000, 'X');
  const auto accept = upgrade::ComputeWebSocketAccept(longKey);
  EXPECT_NE(accept[0], '\0');
}

}  // namespace
}  // namespace aeronet
