#include <gtest/gtest.h>

#ifndef AERONET_WINDOWS
#include <sys/socket.h>
#endif

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/router.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-endpoint.hpp"
#include "aeronet/websocket-handler.hpp"
#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-gateway.hpp"
#endif

using namespace std::chrono_literals;
using namespace aeronet;
using namespace aeronet::websocket;

namespace {

test::TestServer ts;

// Helper to build a valid WebSocket upgrade request. extraHeaders are complete header lines, CRLF included.
std::string BuildUpgradeRequest(std::string_view path,
                                std::string_view key = "dGhlIHNhbXBsZSBub25jZQ==", std::string_view extraHeaders = {}) {
  return std::string("GET ") + std::string(path) +
         " HTTP/1.1\r\n"
         "Host: localhost\r\n"
         "Upgrade: websocket\r\n"
         "Connection: Upgrade\r\n"
         "Sec-WebSocket-Key: " +
         std::string(key) +
         "\r\n"
         "Sec-WebSocket-Version: 13\r\n" +
         std::string(extraHeaders) + "\r\n";
}

std::string BuildUpgradeRequestWithHeaders(std::string_view path, std::string_view extraHeaders) {
  return BuildUpgradeRequest(path, "dGhlIHNhbXBsZSBub25jZQ==", extraHeaders);
}

// Helper to create a masked client frame
vector<std::byte> BuildClientFrame(Opcode opcode, std::string_view text, bool fin = true) {
  vector<std::byte> frame;
  uint8_t firstByte = static_cast<uint8_t>(opcode);
  if (fin) {
    firstByte |= 0x80U;
  }
  frame.push_back(static_cast<std::byte>(firstByte));

  // Mask bit set + length
  if (text.size() < 126) {
    frame.push_back(static_cast<std::byte>(0x80U | text.size()));
  } else if (text.size() < 65536) {
    frame.push_back(static_cast<std::byte>(0x80U | 126U));
    frame.push_back(static_cast<std::byte>((text.size() >> 8U) & 0xFFU));
    frame.push_back(static_cast<std::byte>(text.size() & 0xFFU));
  } else {
    frame.push_back(static_cast<std::byte>(0x80U | 127U));
    for (int idx = 7; idx >= 0; --idx) {
      frame.push_back(static_cast<std::byte>((text.size() >> (static_cast<uint32_t>(idx) * 8U)) & 0xFFU));
    }
  }

  // Masking key (simple key for testing)
  constexpr std::array maskKey{std::byte{0x37}, std::byte{0xfa}, std::byte{0x21}, std::byte{0x3d}};
  for (auto keyByte : maskKey) {
    frame.push_back(keyByte);
  }

  // Masked payload
  for (std::size_t idx = 0; idx < text.size(); ++idx) {
    frame.push_back(static_cast<std::byte>(text[idx]) ^ maskKey[idx % 4]);
  }

  return frame;
}

vector<std::byte> BuildClientTextFrame(std::string_view text, bool fin = true) {
  return BuildClientFrame(Opcode::Text, text, fin);
}

void SendFrame(NativeHandle fd, const vector<std::byte>& frame) {
  test::sendAll(fd, std::string_view(reinterpret_cast<const char*>(frame.data()), frame.size()));
}

// Helper to create a close frame
vector<std::byte> BuildClientCloseFrame(CloseCode code = CloseCode::Normal, std::string_view reason = "") {
  vector<std::byte> payload;
  payload.push_back(static_cast<std::byte>(static_cast<uint8_t>(static_cast<uint16_t>(code) >> 8U) & 0xFFU));
  payload.push_back(static_cast<std::byte>(static_cast<uint16_t>(code) & 0xFFU));
  for (char ch : reason) {
    payload.push_back(static_cast<std::byte>(ch));
  }

  vector<std::byte> frame;
  frame.push_back(static_cast<std::byte>(0x80U | static_cast<uint8_t>(Opcode::Close)));
  frame.push_back(static_cast<std::byte>(0x80U | payload.size()));

  // Masking key
  constexpr std::array maskKey{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
  for (auto keyByte : maskKey) {
    frame.push_back(keyByte);
  }

  // Masked payload
  for (uint32_t idx = 0; idx < payload.size(); ++idx) {
    frame.push_back(payload[idx] ^ maskKey[idx % 4]);
  }

  return frame;
}

// Parse a server frame (unmasked)
struct ServerFrame {
  Opcode opcode{};
  bool fin{false};
  vector<std::byte> payload;
};

std::optional<ServerFrame> ParseServerFrame(std::span<const std::byte> data) {
  if (data.size() < 2) {
    return std::nullopt;
  }

  ServerFrame frame;
  frame.fin = (std::to_integer<uint8_t>(data[0]) & 0x80U) != 0;
  frame.opcode = static_cast<Opcode>(std::to_integer<uint8_t>(data[0]) & 0x0FU);

  bool masked = (std::to_integer<uint8_t>(data[1]) & 0x80U) != 0;
  if (masked) {
    return std::nullopt;  // Server frames should not be masked
  }

  std::size_t payloadLen = std::to_integer<std::size_t>(data[1]) & 0x7FU;
  std::size_t headerSize = 2;

  if (payloadLen == 126) {
    if (data.size() < 4) {
      return std::nullopt;
    }
    payloadLen = (std::to_integer<std::size_t>(data[2]) << 8U) | std::to_integer<std::size_t>(data[3]);
    headerSize = 4;
  } else if (payloadLen == 127) {
    if (data.size() < 10) {
      return std::nullopt;
    }
    payloadLen = 0;
    for (std::size_t idx = 0; idx < 8; ++idx) {
      payloadLen = (payloadLen << 8U) | std::to_integer<std::size_t>(data[2 + idx]);
    }
    headerSize = 10;
  }

  if (data.size() < headerSize + payloadLen) {
    return std::nullopt;
  }

  frame.payload.assign(data.begin() + static_cast<std::ptrdiff_t>(headerSize),
                       data.begin() + static_cast<std::ptrdiff_t>(headerSize + payloadLen));
  return frame;
}

// Receives bytes until they hold one complete server frame. Returns the raw bytes (empty on timeout / close).
std::string ReceiveServerFrameBytes(NativeHandle fd, std::chrono::milliseconds timeout) {
  test::setRecvTimeout(fd, timeout);
  std::string raw;
  std::array<char, 16384> buf{};
  while (!ParseServerFrame(std::span<const std::byte>(reinterpret_cast<const std::byte*>(raw.data()), raw.size()))) {
    const auto nb = ::recv(fd, buf.data(), buf.size(), 0);
    if (nb <= 0) {
      return {};
    }
    raw.append(buf.data(), static_cast<std::size_t>(nb));
  }
  return raw;
}

std::string PayloadToString(std::span<const std::byte> payload) {
  std::string result;
  for (auto byte : payload) {
    result.push_back(static_cast<char>(byte));
  }
  return result;
}

class WebSocketTest : public ::testing::Test {
 protected:
  struct CloseInfo {
    bool received{false};
    CloseCode code{CloseCode::Normal};
    std::string reason;
  };

  void SetUp() override {
    std::scoped_lock lock(_mutex);
    _receivedMessages.clear();
    _close = {};
  }

  // Called by the WebSocket callbacks, from the server event loop thread.
  void recordMessage(std::span<const std::byte> payload, bool isBinary) {
    std::scoped_lock lock(_mutex);
    _receivedMessages.emplace_back(PayloadToString(payload), isBinary);
  }

  void recordClose(CloseCode code, std::string_view reason) {
    std::scoped_lock lock(_mutex);
    _close = CloseInfo{.received = true, .code = code, .reason = std::string(reason)};
  }

  // Snapshots for the test thread.
  vector<std::pair<std::string, bool>> receivedMessages() const {
    std::scoped_lock lock(_mutex);
    return _receivedMessages;
  }

  CloseInfo closeInfo() const {
    std::scoped_lock lock(_mutex);
    return _close;
  }

 private:
  // Captured by the callbacks (server event loop thread) and checked by the test thread.
  mutable std::mutex _mutex;
  vector<std::pair<std::string, bool>> _receivedMessages;  // payload, isBinary
  CloseInfo _close;
};

TEST_F(WebSocketTest, UpgradeSuccessful) {
  // Register a WebSocket endpoint
  ts.postRouterUpdate([this](Router& router) {
    router.setWebSocket("/ws", WebSocketEndpoint::WithCallbacks(WebSocketCallbacks{
                                   .onMessage = [this](std::span<const std::byte> payload,
                                                       bool isBinary) { recordMessage(payload, isBinary); },
                                   .onPing = {},
                                   .onPong = {},
                                   .onClose = {},
                                   .onError = {},
                               }));
  });

  // Connect and send upgrade request
  test::ClientConnection conn(ts.port());
  std::string upgradeReq = BuildUpgradeRequest("/ws");
  test::sendAll(conn.fd(), upgradeReq);

  // Read response
  std::string response = test::recvWithTimeout(conn.fd(), std::chrono::seconds{1}, 129UL);

  // Verify 101 response
  EXPECT_TRUE(response.starts_with("HTTP/1.1 101")) << "Response: " << response;
  EXPECT_TRUE(response.contains(MakeHttp1HeaderLine(http::Upgrade, websocket::UpgradeValue)))
      << "Response: " << response;
  EXPECT_TRUE(response.contains("sec-websocket-accept:")) << "Response: " << response;
}

TEST_F(WebSocketTest, UpgradeWithInvalidKey) {
  ts.postRouterUpdate([](Router& router) {
    router.setWebSocket("/ws", WebSocketEndpoint::WithCallbacks({
                                   .onMessage = {},
                                   .onPing = {},
                                   .onPong = {},
                                   .onClose = {},
                                   .onError = {},
                               }));
  });

  test::ClientConnection conn(ts.port());
  // Invalid key (too short)
  std::string upgradeReq = BuildUpgradeRequest("/ws", "shortkey");
  test::sendAll(conn.fd(), upgradeReq);

  std::string response = test::recvWithTimeout(conn.fd(), 500ms);  // NOLINT(misc-include-cleaner)

  // Should get 400 Bad Request
  EXPECT_TRUE(response.starts_with("HTTP/1.1 400")) << "Response: " << response;
}

TEST_F(WebSocketTest, UpgradeNonWebSocketPath) {
  ts.postRouterUpdate([](Router& router) {
    router.setWebSocket("/ws", WebSocketEndpoint::WithCallbacks({
                                   .onMessage = {},
                                   .onPing = {},
                                   .onPong = {},
                                   .onClose = {},
                                   .onError = {},
                               }));
  });

  test::ClientConnection conn(ts.port());
  // Request upgrade on path without WebSocket handler
  std::string upgradeReq = BuildUpgradeRequest("/other");
  test::sendAll(conn.fd(), upgradeReq);

  std::string response = test::recvWithTimeout(conn.fd(), 1000ms, 1531UL);

  // Should get 404 Not Found (no handler for /other)
  EXPECT_TRUE(response.starts_with("HTTP/1.1 404")) << "Response: " << response;
}

TEST_F(WebSocketTest, SendAndReceiveTextMessage) {
  ts.postRouterUpdate([this](Router& router) {
    router.setWebSocket("/echo", WebSocketEndpoint::WithFactory([this](const HttpRequestView& /*req*/) {
                          auto handler = std::make_unique<WebSocketHandler>();
                          handler->setCallbacks(WebSocketCallbacks{
                              .onMessage =
                                  [this, handler = handler.get()](std::span<const std::byte> payload, bool isBinary) {
                                    recordMessage(payload, isBinary);
                                    // Echo back
                                    if (!isBinary) {
                                      handler->sendText(PayloadToString(payload));
                                    }
                                  },
                              .onPing = {},
                              .onPong = {},
                              .onClose = {},
                              .onError = {},
                          });
                          return handler;
                        }));
  });

  test::ClientConnection conn(ts.port());

  // Upgrade
  test::sendAll(conn.fd(), BuildUpgradeRequest("/echo"));
  std::string upgradeResponse = test::recvWithTimeout(conn.fd(), 1000ms, 129UL);
  ASSERT_TRUE(upgradeResponse.starts_with("HTTP/1.1 101"));

  // Send a text frame
  auto textFrame = BuildClientTextFrame("Hello, WebSocket!");
  test::sendAll(conn.fd(), std::string_view(reinterpret_cast<const char*>(textFrame.data()), textFrame.size()));

  // Wait for echo response
  std::this_thread::sleep_for(50ms);  // NOLINT(misc-include-cleaner)

  // Read response frame
  std::string response = test::recvWithTimeout(conn.fd(), 1000ms, 19UL);

  // Parse the frame
  std::span<const std::byte> responseData(reinterpret_cast<const std::byte*>(response.data()), response.size());
  auto frame = ParseServerFrame(responseData);

  ASSERT_TRUE(frame.has_value()) << "Failed to parse server frame, raw size: " << response.size();
  EXPECT_EQ(frame.value_or(ServerFrame{}).opcode, Opcode::Text);
  EXPECT_TRUE(frame.value_or(ServerFrame{}).fin);
  EXPECT_EQ(PayloadToString(frame.value_or(ServerFrame{}).payload), "Hello, WebSocket!");

  // Verify server received our message
  const auto receivedMessages = this->receivedMessages();
  ASSERT_EQ(receivedMessages.size(), 1);
  EXPECT_EQ(receivedMessages[0].first, "Hello, WebSocket!");
  EXPECT_FALSE(receivedMessages[0].second);  // Text, not binary
}

// A frame bigger than the loopback MSS is always received in several reads: the message must be delivered once and
// intact, and the connection must stay usable (the bytes of an incomplete frame used to be processed twice).
TEST_F(WebSocketTest, LargeFrameSplitAcrossReadsIsEchoedIntact) {
  ts.postRouterUpdate([](Router& router) {
    router.setWebSocket("/echo-large", WebSocketEndpoint::WithFactory([](const HttpRequestView& /*req*/) {
                          auto handler = std::make_unique<WebSocketHandler>();
                          handler->setCallbacks(WebSocketCallbacks{
                              .onMessage = [handler = handler.get()](
                                               std::span<const std::byte> payload,
                                               bool /*isBinary*/) { handler->sendText(PayloadToString(payload)); },
                              .onPing = {},
                              .onPong = {},
                              .onClose = {},
                              .onError = {},
                          });
                          return handler;
                        }));
  });

  test::ClientConnection conn(ts.port());
  test::sendAll(conn.fd(), BuildUpgradeRequest("/echo-large"));
  ASSERT_TRUE(test::recvWithTimeout(conn.fd(), 1000ms, 129UL).starts_with("HTTP/1.1 101"));

  std::string text(100000, '\0');
  for (std::size_t pos = 0; pos < text.size(); ++pos) {
    text[pos] = static_cast<char>('a' + (pos % 26));
  }
  for (int round = 0; round < 3; ++round) {
    const auto frame = BuildClientTextFrame(text);
    test::sendAll(conn.fd(), std::string_view(reinterpret_cast<const char*>(frame.data()), frame.size()), 2000ms);
    const std::string raw = ReceiveServerFrameBytes(conn.fd(), 2000ms);
    const auto echo =
        ParseServerFrame(std::span<const std::byte>(reinterpret_cast<const std::byte*>(raw.data()), raw.size()));
    ASSERT_TRUE(echo.has_value()) << "round " << round;
    EXPECT_EQ(echo.value_or(ServerFrame{}).opcode, Opcode::Text);
    EXPECT_EQ(PayloadToString(echo.value_or(ServerFrame{}).payload), text) << "round " << round;
  }
}

#ifdef AERONET_ENABLE_ZLIB

namespace {

// Masked client frame carrying arbitrary payload bytes (rsv1: permessage-deflate compressed message).
vector<std::byte> BuildClientFrame(std::string_view payload, Opcode opcode, bool rsv1) {
  vector<std::byte> frame;
  frame.push_back(static_cast<std::byte>(0x80U | (rsv1 ? 0x40U : 0U) | static_cast<uint8_t>(opcode)));
  if (payload.size() < 126) {
    frame.push_back(static_cast<std::byte>(0x80U | payload.size()));
  } else if (payload.size() < 65536) {
    frame.push_back(static_cast<std::byte>(0x80U | 126U));
    frame.push_back(static_cast<std::byte>((payload.size() >> 8U) & 0xFFU));
    frame.push_back(static_cast<std::byte>(payload.size() & 0xFFU));
  } else {
    frame.push_back(static_cast<std::byte>(0x80U | 127U));
    for (int idx = 7; idx >= 0; --idx) {
      frame.push_back(static_cast<std::byte>((payload.size() >> (static_cast<uint32_t>(idx) * 8U)) & 0xFFU));
    }
  }
  constexpr std::array maskKey{std::byte{0x5a}, std::byte{0x1c}, std::byte{0xe3}, std::byte{0x07}};
  for (auto keyByte : maskKey) {
    frame.push_back(keyByte);
  }
  for (std::size_t idx = 0; idx < payload.size(); ++idx) {
    frame.push_back(static_cast<std::byte>(payload[idx]) ^ maskKey[idx % 4]);
  }
  return frame;
}

// Receives the HTTP response head of an upgrade request (up to the empty line).
std::string ReceiveResponseHead(NativeHandle fd, std::chrono::milliseconds timeout) {
  test::setRecvTimeout(fd, timeout);
  std::string head;
  char ch{};
  while (!head.ends_with("\r\n\r\n") && ::recv(fd, &ch, 1, 0) == 1) {
    head.push_back(ch);
  }
  return head;
}

}  // namespace

// permessage-deflate interoperability with a standard client (raw DEFLATE payloads, RFC 7692), for an endpoint whose
// factory does not configure compression itself: the server enables it on the handler, keeping its callbacks.
TEST_F(WebSocketTest, CompressedMessageFromStandardClientIsEchoedCompressed) {
  ts.postRouterUpdate([](Router& router) {
    WebSocketConfig config;
    config.deflateConfig.enabled = true;
    config.deflateConfig.minCompressSize = 16;
    auto endpoint = WebSocketEndpoint::WithFactory([config](const HttpRequestView& /*req*/) {
      auto handler = std::make_unique<WebSocketHandler>(config);
      handler->setCallbacks(WebSocketCallbacks{
          .onMessage = [handler = handler.get()](std::span<const std::byte> payload,
                                                 bool /*isBinary*/) { handler->sendText(PayloadToString(payload)); },
          .onPing = {},
          .onPong = {},
          .onClose = {},
          .onError = {},
      });
      return handler;
    });
    endpoint.config = config;
    router.setWebSocket("/echo-deflate", std::move(endpoint));
  });

  test::ClientConnection conn(ts.port());
  std::string upgrade = BuildUpgradeRequest("/echo-deflate");
  upgrade.insert(upgrade.size() - 2,
                 "Sec-WebSocket-Extensions: permessage-deflate; client_no_context_takeover; "
                 "server_no_context_takeover\r\n");
  test::sendAll(conn.fd(), upgrade);
  const std::string upgradeResponse = ReceiveResponseHead(conn.fd(), 1000ms);
  ASSERT_TRUE(upgradeResponse.starts_with("HTTP/1.1 101")) << upgradeResponse;
  ASSERT_TRUE(upgradeResponse.contains("permessage-deflate")) << upgradeResponse;

  std::string text;
  for (int idx = 0; idx < 50; ++idx) {
    text += R"({"id":)" + std::to_string(idx) + R"(,"tags":["benchmark","compression"]})";
  }

  // Compress like any RFC 7692 client: raw deflate, sync flush, trailing 0x00 0x00 0xff 0xff removed.
  zstream deflater{};
  ASSERT_EQ(ZDeflateInit2(deflater, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY), Z_OK);
  std::string compressed(text.size() + 256, '\0');
  ZSetInput(deflater, text);
  ZSetOutput(deflater, compressed.data(), compressed.size());
  ASSERT_EQ(ZDeflate(deflater, Z_SYNC_FLUSH), Z_OK);
  compressed.resize(compressed.size() - deflater.avail_out - 4);
  ZDeflateEnd(deflater);

  const auto frame = BuildClientFrame(compressed, Opcode::Text, true);
  test::sendAll(conn.fd(), std::string_view(reinterpret_cast<const char*>(frame.data()), frame.size()));

  const std::string raw = ReceiveServerFrameBytes(conn.fd(), 2000ms);
  ASSERT_FALSE(raw.empty()) << "no echo received";
  EXPECT_NE(static_cast<uint8_t>(raw[0]) & 0x40U, 0U) << "echo is not compressed (RSV1 not set)";
  const auto echo =
      ParseServerFrame(std::span<const std::byte>(reinterpret_cast<const std::byte*>(raw.data()), raw.size()));
  ASSERT_TRUE(echo.has_value());

  // Decompress like any RFC 7692 client: append the trailer and inflate raw data.
  std::string echoPayload = PayloadToString(echo.value_or(ServerFrame{}).payload);
  echoPayload.append("\x00\x00\xff\xff", 4);
  zstream inflater{};
  ASSERT_EQ(ZInflateInit2(inflater, -15), Z_OK);
  std::string decompressed(text.size() * 2, '\0');
  ZSetInput(inflater, echoPayload);
  ZSetOutput(inflater, decompressed.data(), decompressed.size());
  const auto ret = ZInflate(inflater, Z_SYNC_FLUSH);
  EXPECT_TRUE(ret == Z_OK || ret == Z_BUF_ERROR) << ret;
  decompressed.resize(decompressed.size() - inflater.avail_out);
  ZInflateEnd(inflater);
  EXPECT_EQ(decompressed, text);
}
#endif

TEST_F(WebSocketTest, CloseHandshake) {
  ts.postRouterUpdate([this](Router& router) {
    router.setWebSocket("/ws",
                        WebSocketEndpoint::WithCallbacks(WebSocketCallbacks{
                            .onMessage = {},
                            .onPing = {},
                            .onPong = {},
                            .onClose = [this](CloseCode code, std::string_view reason) { recordClose(code, reason); },
                            .onError = {},
                        }));
  });

  test::ClientConnection conn(ts.port());

  // Upgrade
  test::sendAll(conn.fd(), BuildUpgradeRequest("/ws"));
  std::string upgradeResponse = test::recvWithTimeout(conn.fd(), 1000ms, 129UL);
  ASSERT_TRUE(upgradeResponse.starts_with("HTTP/1.1 101"));

  // Send close frame
  auto closeFrame = BuildClientCloseFrame(CloseCode::Normal, "goodbye");
  test::sendAll(conn.fd(), std::string_view(reinterpret_cast<const char*>(closeFrame.data()), closeFrame.size()));

  // Wait for close response
  std::this_thread::sleep_for(50ms);

  // Read response
  std::string rawResponse = test::recvWithTimeout(conn.fd(), 1000ms, 11UL);
  std::span<const std::byte> responseData(reinterpret_cast<const std::byte*>(rawResponse.data()), rawResponse.size());
  auto frame = ParseServerFrame(responseData);

  // Server should send close frame back
  ASSERT_TRUE(frame.has_value()) << "Failed to parse close response";
  EXPECT_EQ(frame.value_or(ServerFrame{}).opcode, Opcode::Close);

  // Verify callback was invoked
  const auto close = closeInfo();
  EXPECT_TRUE(close.received);
  EXPECT_EQ(close.code, CloseCode::Normal);
  EXPECT_EQ(close.reason, "goodbye");

  // The server closes the TCP connection first (RFC 6455 section 7.1.1): the client sees EOF without closing.
  EXPECT_TRUE(test::WaitForPeerClose(conn.fd(), 500ms));
}

TEST_F(WebSocketTest, WithConfigAndCallbacksCustomMaxMessageSize) {
  WebSocketConfig config;
  config.maxMessageSize = 100;  // Small limit for testing

  ts.postRouterUpdate([this, config](Router& router) {
    router.setWebSocket("/ws", WebSocketEndpoint::WithConfigAndCallbacks(
                                   config, WebSocketCallbacks{
                                               .onMessage = [this](std::span<const std::byte> payload,
                                                                   bool isBinary) { recordMessage(payload, isBinary); },
                                               .onPing = {},
                                               .onPong = {},
                                               .onClose = {},
                                               .onError = {},
                                           }));
  });

  test::ClientConnection conn(ts.port());

  // Upgrade
  test::sendAll(conn.fd(), BuildUpgradeRequest("/ws"));
  std::string upgradeResponse = test::recvWithTimeout(conn.fd(), 1000ms, 129UL);
  ASSERT_TRUE(upgradeResponse.starts_with("HTTP/1.1 101"));

  // Send a small message (should work)
  auto smallFrame = BuildClientTextFrame("Small message");
  test::sendAll(conn.fd(), std::string_view(reinterpret_cast<const char*>(smallFrame.data()), smallFrame.size()));

  // Wait for processing
  std::this_thread::sleep_for(50ms);

  const auto receivedMessages = this->receivedMessages();
  ASSERT_EQ(receivedMessages.size(), 1);
  EXPECT_EQ(receivedMessages[0].first, "Small message");
}

// ============================================================================
// Idle and close timeouts
// ============================================================================

// Server dedicated to the timeout tests, with a frequent maintenance sweep.
HttpServerConfig TimeoutServerConfig(std::chrono::milliseconds keepAliveTimeout) {
  HttpServerConfig cfg;
  cfg.withKeepAliveTimeout(keepAliveTimeout).withPollInterval(10ms);
  return cfg;
}

// Endpoint echoing text messages, closing the connection when it receives "close".
WebSocketEndpoint EchoEndpoint(WebSocketConfig config) {
  return WebSocketEndpoint::WithFactory([config](const HttpRequestView& /*req*/) {
    auto handler = std::make_unique<WebSocketHandler>(config);
    handler->setCallbacks(WebSocketCallbacks{
        .onMessage =
            [handler = handler.get()](std::span<const std::byte> payload, bool /*isBinary*/) {
              const std::string text = PayloadToString(payload);
              if (text == "close") {
                handler->sendClose(CloseCode::Normal, "bye");
              } else {
                handler->sendText(text);
              }
            },
        .onPing = {},
        .onPong = {},
        .onClose = {},
        .onError = {},
    });
    return handler;
  });
}

std::optional<ServerFrame> ReceiveServerFrame(NativeHandle fd, std::chrono::milliseconds timeout) {
  const std::string raw = ReceiveServerFrameBytes(fd, timeout);
  return ParseServerFrame(std::span<const std::byte>(reinterpret_cast<const std::byte*>(raw.data()), raw.size()));
}

void Upgrade(NativeHandle fd, std::string_view path) {
  test::sendAll(fd, BuildUpgradeRequest(path));
  const std::string upgradeResponse = test::recvWithTimeout(fd, 1000ms, 129UL);
  ASSERT_TRUE(upgradeResponse.starts_with("HTTP/1.1 101")) << upgradeResponse;
}

TEST(WebSocketTimeouts, UpgradedConnectionOutlivesKeepAliveTimeout) {
  test::TestServer server(TimeoutServerConfig(100ms));
  server.postRouterUpdate([](Router& router) { router.setWebSocket("/echo", EchoEndpoint(WebSocketConfig{})); });

  test::ClientConnection conn(server.port());
  Upgrade(conn.fd(), "/echo");

  // keepAliveTimeout bounds the idleness between HTTP requests, not WebSocket connections.
  std::this_thread::sleep_for(400ms);
  SendFrame(conn.fd(), BuildClientTextFrame("still there?"));
  const auto echo = ReceiveServerFrame(conn.fd(), 1000ms);
  ASSERT_TRUE(echo.has_value());
  EXPECT_EQ(echo->opcode, Opcode::Text);
  EXPECT_EQ(PayloadToString(echo->payload), "still there?");
}

TEST(WebSocketTimeouts, IdleConnectionIsPingedThenClosed) {
  test::TestServer server(TimeoutServerConfig(10s));
  WebSocketConfig config;
  config.idleTimeout = 400ms;
  server.postRouterUpdate([config](Router& router) { router.setWebSocket("/echo", EchoEndpoint(config)); });

  test::ClientConnection conn(server.port());
  Upgrade(conn.fd(), "/echo");

  // A Ping after half of the idle timeout, then the connection is closed when the peer does not answer.
  const auto ping = ReceiveServerFrame(conn.fd(), 1000ms);
  ASSERT_TRUE(ping.has_value());
  EXPECT_EQ(ping->opcode, Opcode::Ping);
  EXPECT_TRUE(test::WaitForPeerClose(conn.fd(), 2000ms));
}

TEST(WebSocketTimeouts, PongsKeepIdleConnectionOpen) {
  test::TestServer server(TimeoutServerConfig(10s));
  WebSocketConfig config;
  config.idleTimeout = 1000ms;  // a Ping after 500 ms of idleness, to answer within 500 ms
  server.postRouterUpdate([config](Router& router) { router.setWebSocket("/echo", EchoEndpoint(config)); });

  test::ClientConnection conn(server.port());
  Upgrade(conn.fd(), "/echo");

  // Answer the Pings for more than an idle timeout, like a browser does.
  for (int pingIdx = 0; pingIdx < 3; ++pingIdx) {
    const auto ping = ReceiveServerFrame(conn.fd(), 2000ms);
    ASSERT_TRUE(ping.has_value());
    ASSERT_EQ(ping->opcode, Opcode::Ping);
    SendFrame(conn.fd(), BuildClientFrame(Opcode::Pong, PayloadToString(ping->payload)));
  }
  SendFrame(conn.fd(), BuildClientTextFrame("alive"));
  auto frame = ReceiveServerFrame(conn.fd(), 1000ms);
  // A Ping may be sent before the echo.
  if (frame.has_value() && frame->opcode == Opcode::Ping) {
    frame = ReceiveServerFrame(conn.fd(), 1000ms);
  }
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->opcode, Opcode::Text);
  EXPECT_EQ(PayloadToString(frame->payload), "alive");
}

TEST(WebSocketTimeouts, CloseTimeoutClosesConnectionWithoutPeerClose) {
  test::TestServer server(TimeoutServerConfig(10s));
  WebSocketConfig config;
  config.idleTimeout = 0ms;
  config.closeTimeout = 200ms;
  server.postRouterUpdate([config](Router& router) { router.setWebSocket("/echo", EchoEndpoint(config)); });

  test::ClientConnection conn(server.port());
  Upgrade(conn.fd(), "/echo");

  SendFrame(conn.fd(), BuildClientTextFrame("close"));
  const auto close = ReceiveServerFrame(conn.fd(), 1000ms);
  ASSERT_TRUE(close.has_value());
  EXPECT_EQ(close->opcode, Opcode::Close);
  // The client never answers the Close frame: the server gives up after closeTimeout.
  EXPECT_TRUE(test::WaitForPeerClose(conn.fd(), 2000ms));
}

// ============================================================================
// Upgrade: middleware, origin checks, CORS, factory
// ============================================================================

WebSocketEndpoint SilentEndpoint() {
  return WebSocketEndpoint::WithCallbacks(WebSocketCallbacks{
      .onMessage = {},
      .onPing = {},
      .onPong = {},
      .onClose = {},
      .onError = {},
  });
}

// Head of the response to an upgrade request on the shared server.
std::string UpgradeResponse(std::string_view path, std::string_view extraHeaders) {
  test::ClientConnection conn(ts.port());
  test::sendAll(conn.fd(), BuildUpgradeRequestWithHeaders(path, extraHeaders));
  test::setRecvTimeout(conn.fd(), 1000ms);
  std::string head;
  std::array<char, 1024> buf{};
  while (!head.contains(http::DoubleCRLF)) {
    const auto nb = ::recv(conn.fd(), buf.data(), buf.size(), 0);
    if (nb <= 0) {
      break;
    }
    head.append(buf.data(), static_cast<std::size_t>(nb));
  }
  return head;
}

TEST_F(WebSocketTest, UpgradeRunsRequestMiddleware) {
  ts.postRouterUpdate([](Router& router) {
    router.setWebSocket("/ws-middleware", SilentEndpoint()).before([](HttpRequestView& req) {
      if (req.headerValueOrEmpty(http::Authorization) != "Bearer token") {
        return MiddlewareResult::ShortCircuit(req.makeResponse(http::StatusCodeUnauthorized));
      }
      return MiddlewareResult::Continue();
    });
  });

  EXPECT_TRUE(UpgradeResponse("/ws-middleware", "").starts_with("HTTP/1.1 401"));
  EXPECT_TRUE(UpgradeResponse("/ws-middleware", "Authorization: Bearer token\r\n").starts_with("HTTP/1.1 101"));
}

TEST_F(WebSocketTest, UpgradeChecksOriginWithoutCorsPolicy) {
  ts.postRouterUpdate([](Router& router) { router.setWebSocket("/ws-origin", SilentEndpoint()); });

  // No Origin: not a browser.
  EXPECT_TRUE(UpgradeResponse("/ws-origin", "").starts_with("HTTP/1.1 101"));
  // Same origin as the Host header (localhost), default port omitted or not.
  EXPECT_TRUE(UpgradeResponse("/ws-origin", "Origin: http://localhost\r\n").starts_with("HTTP/1.1 101"));
  EXPECT_TRUE(UpgradeResponse("/ws-origin", "Origin: http://LocalHost:80\r\n").starts_with("HTTP/1.1 101"));
  EXPECT_TRUE(UpgradeResponse("/ws-origin", "Origin: https://localhost:443\r\n").starts_with("HTTP/1.1 101"));
  // Cross-site WebSocket hijacking attempts.
  EXPECT_TRUE(UpgradeResponse("/ws-origin", "Origin: https://evil.example\r\n").starts_with("HTTP/1.1 403"));
  EXPECT_TRUE(UpgradeResponse("/ws-origin", "Origin: http://localhost:8080\r\n").starts_with("HTTP/1.1 403"));
  EXPECT_TRUE(UpgradeResponse("/ws-origin", "Origin: null\r\n").starts_with("HTTP/1.1 403"));
}

TEST_F(WebSocketTest, UpgradeAppliesCorsPolicyToOrigin) {
  ts.postRouterUpdate([](Router& router) {
    CorsPolicy cors;
    cors.allowOrigin("https://app.example.com");
    router.setWebSocket("/ws-cors", SilentEndpoint()).cors(std::move(cors));
  });

  EXPECT_TRUE(UpgradeResponse("/ws-cors", "Origin: https://app.example.com\r\n").starts_with("HTTP/1.1 101"));
  EXPECT_TRUE(UpgradeResponse("/ws-cors", "Origin: https://evil.example\r\n").starts_with("HTTP/1.1 403"));
  // The policy replaces the same-origin rule.
  EXPECT_TRUE(UpgradeResponse("/ws-cors", "Origin: http://localhost\r\n").starts_with("HTTP/1.1 403"));
  EXPECT_TRUE(UpgradeResponse("/ws-cors", "").starts_with("HTTP/1.1 101"));
}

TEST_F(WebSocketTest, FactoryCanRefuseUpgrade) {
  ts.postRouterUpdate([](Router& router) {
    router.setWebSocket("/rooms/{room}", WebSocketEndpoint::WithFactory([](const HttpRequestView& req) {
                          // Path parameters are available to the factory.
                          if (req.pathParamValueOrEmpty("room") != "lobby") {
                            return std::unique_ptr<WebSocketHandler>();
                          }
                          return std::make_unique<WebSocketHandler>();
                        }));
  });

  EXPECT_TRUE(UpgradeResponse("/rooms/lobby", "").starts_with("HTTP/1.1 101"));
  EXPECT_TRUE(UpgradeResponse("/rooms/private", "").starts_with("HTTP/1.1 403"));
}

TEST_F(WebSocketTest, EndpointWithoutFactoryUsesADefaultHandler) {
  ts.postRouterUpdate([](Router& router) {
    WebSocketEndpoint endpoint;
    endpoint.config.maxMessageSize = 1024;
    router.setWebSocket("/ws-default", std::move(endpoint));
  });

  EXPECT_TRUE(UpgradeResponse("/ws-default", "").starts_with("HTTP/1.1 101"));
}

TEST_F(WebSocketTest, ThrowingFactoryAnswers500) {
  ts.postRouterUpdate([](Router& router) {
    router.setWebSocket("/std",
                        WebSocketEndpoint::WithFactory([](const HttpRequestView&) -> std::unique_ptr<WebSocketHandler> {
                          throw std::runtime_error("factory failure");
                        }));
    router.setWebSocket("/other",
                        WebSocketEndpoint::WithFactory([](const HttpRequestView&) -> std::unique_ptr<WebSocketHandler> {
                          throw 42;  // NOLINT(hicpp-exception-baseclass)
                        }));
  });

  const std::string stdResponse = UpgradeResponse("/std", "");
  EXPECT_TRUE(stdResponse.starts_with("HTTP/1.1 500")) << stdResponse;
  EXPECT_FALSE(stdResponse.contains("factory failure"));
  EXPECT_TRUE(UpgradeResponse("/other", "").starts_with("HTTP/1.1 500"));
}

TEST_F(WebSocketTest, RequestTimeoutOfRouteDoesNotCloseWebSocket) {
  ts.postRouterUpdate(
      [](Router& router) { router.setWebSocket("/echo-timeout", EchoEndpoint(WebSocketConfig{})).timeout(100ms); });

  test::ClientConnection conn(ts.port());
  Upgrade(conn.fd(), "/echo-timeout");
  std::this_thread::sleep_for(300ms);
  SendFrame(conn.fd(), BuildClientTextFrame("ping"));
  const auto echo = ReceiveServerFrame(conn.fd(), 1000ms);
  ASSERT_TRUE(echo.has_value());
  EXPECT_EQ(PayloadToString(echo->payload), "ping");
}

}  // namespace
