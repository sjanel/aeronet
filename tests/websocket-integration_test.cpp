#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aeronet/http-server-config.hpp"
#include "aeronet/router.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-endpoint.hpp"
#include "aeronet/websocket-handler.hpp"

using namespace std::chrono_literals;
using namespace aeronet;
using namespace aeronet::websocket;

namespace {

// Helper to build a valid WebSocket upgrade request
std::string BuildUpgradeRequest(std::string_view path, std::string_view key = "dGhlIHNhbXBsZSBub25jZQ==") {
  return std::string("GET ") + std::string(path) +
         " HTTP/1.1\r\n"
         "Host: localhost\r\n"
         "Upgrade: websocket\r\n"
         "Connection: Upgrade\r\n"
         "Sec-WebSocket-Key: " +
         std::string(key) +
         "\r\n"
         "Sec-WebSocket-Version: 13\r\n\r\n";
}

// Helper to create a masked client frame
std::vector<std::byte> BuildClientTextFrame(std::string_view text, bool fin = true) {
  std::vector<std::byte> frame;
  uint8_t firstByte = static_cast<uint8_t>(Opcode::Text);
  if (fin) {
    firstByte |= 0x80;
  }
  frame.push_back(static_cast<std::byte>(firstByte));

  // Mask bit set + length
  if (text.size() < 126) {
    frame.push_back(static_cast<std::byte>(0x80 | text.size()));
  } else if (text.size() < 65536) {
    frame.push_back(static_cast<std::byte>(0x80 | 126));
    frame.push_back(static_cast<std::byte>((text.size() >> 8) & 0xFF));
    frame.push_back(static_cast<std::byte>(text.size() & 0xFF));
  } else {
    frame.push_back(static_cast<std::byte>(0x80 | 127));
    for (int idx = 7; idx >= 0; --idx) {
      frame.push_back(static_cast<std::byte>((text.size() >> (idx * 8)) & 0xFF));
    }
  }

  // Masking key (simple key for testing)
  std::array<std::byte, 4> maskKey = {std::byte{0x37}, std::byte{0xfa}, std::byte{0x21}, std::byte{0x3d}};
  for (auto keyByte : maskKey) {
    frame.push_back(keyByte);
  }

  // Masked payload
  for (std::size_t idx = 0; idx < text.size(); ++idx) {
    frame.push_back(static_cast<std::byte>(text[idx]) ^ maskKey[idx % 4]);
  }

  return frame;
}

// Helper to create a close frame
std::vector<std::byte> BuildClientCloseFrame(CloseCode code = CloseCode::Normal, std::string_view reason = "") {
  std::vector<std::byte> payload;
  payload.push_back(static_cast<std::byte>((static_cast<uint16_t>(code) >> 8) & 0xFF));
  payload.push_back(static_cast<std::byte>(static_cast<uint16_t>(code) & 0xFF));
  for (char ch : reason) {
    payload.push_back(static_cast<std::byte>(ch));
  }

  std::vector<std::byte> frame;
  frame.push_back(static_cast<std::byte>(0x80 | static_cast<uint8_t>(Opcode::Close)));
  frame.push_back(static_cast<std::byte>(0x80 | payload.size()));

  // Masking key
  std::array<std::byte, 4> maskKey = {std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
  for (auto keyByte : maskKey) {
    frame.push_back(keyByte);
  }

  // Masked payload
  for (std::size_t idx = 0; idx < payload.size(); ++idx) {
    frame.push_back(payload[idx] ^ maskKey[idx % 4]);
  }

  return frame;
}

// Parse a server frame (unmasked)
struct ServerFrame {
  Opcode opcode{};
  bool fin{false};
  std::vector<std::byte> payload;
};

std::optional<ServerFrame> ParseServerFrame(std::span<const std::byte> data) {
  if (data.size() < 2) {
    return std::nullopt;
  }

  ServerFrame frame;
  frame.fin = (std::to_integer<uint8_t>(data[0]) & 0x80) != 0;
  frame.opcode = static_cast<Opcode>(std::to_integer<uint8_t>(data[0]) & 0x0F);

  bool masked = (std::to_integer<uint8_t>(data[1]) & 0x80) != 0;
  if (masked) {
    return std::nullopt;  // Server frames should not be masked
  }

  std::size_t payloadLen = std::to_integer<std::size_t>(data[1]) & 0x7F;
  std::size_t headerSize = 2;

  if (payloadLen == 126) {
    if (data.size() < 4) {
      return std::nullopt;
    }
    payloadLen = (std::to_integer<std::size_t>(data[2]) << 8) | std::to_integer<std::size_t>(data[3]);
    headerSize = 4;
  } else if (payloadLen == 127) {
    if (data.size() < 10) {
      return std::nullopt;
    }
    payloadLen = 0;
    for (std::size_t idx = 0; idx < 8; ++idx) {
      payloadLen = (payloadLen << 8) | std::to_integer<std::size_t>(data[2 + idx]);
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

std::string PayloadToString(const std::vector<std::byte>& payload) {
  std::string result;
  for (auto byte : payload) {
    result.push_back(static_cast<char>(byte));
  }
  return result;
}

class WebSocketTest : public ::testing::Test {
 protected:
  void SetUp() override {
    _receivedMessages.clear();
    _receivedPings.clear();
    _closeReceived = false;
  }

  // Capture callbacks for verification
  std::vector<std::pair<std::string, bool>> _receivedMessages;  // payload, isBinary
  std::vector<std::string> _receivedPings;
  bool _closeReceived{false};
  CloseCode _closeCode{CloseCode::Normal};
  std::string _closeReason;
};

TEST_F(WebSocketTest, UpgradeSuccessful) {
  test::TestServer ts(HttpServerConfig{});

  // Register a WebSocket endpoint
  ts.postRouterUpdate([this](Router& router) {
    router.setWebSocket("/ws", WebSocketEndpoint::WithCallbacks(WebSocketCallbacks{
                                   .onMessage =
                                       [this](std::span<const std::byte> payload, bool isBinary) {
                                         _receivedMessages.emplace_back(
                                             PayloadToString({payload.begin(), payload.end()}), isBinary);
                                       },
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
  std::string response = test::recvWithTimeout(conn.fd(), 500ms);

  // Verify 101 response
  EXPECT_TRUE(response.contains("HTTP/1.1 101")) << "Response: " << response;
  EXPECT_TRUE(response.contains("Upgrade: websocket")) << "Response: " << response;
  EXPECT_TRUE(response.contains("Sec-WebSocket-Accept:")) << "Response: " << response;
}

TEST_F(WebSocketTest, UpgradeWithInvalidKey) {
  test::TestServer ts(HttpServerConfig{});

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

  std::string response = test::recvWithTimeout(conn.fd(), 500ms);

  // Should get 400 Bad Request
  EXPECT_TRUE(response.contains("HTTP/1.1 400")) << "Response: " << response;
}

TEST_F(WebSocketTest, UpgradeNonWebSocketPath) {
  test::TestServer ts(HttpServerConfig{});

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

  std::string response = test::recvWithTimeout(conn.fd(), 500ms);

  // Should get 404 Not Found (no handler for /other)
  EXPECT_TRUE(response.find("HTTP/1.1 404") != std::string::npos) << "Response: " << response;
}

TEST_F(WebSocketTest, SendAndReceiveTextMessage) {
  test::TestServer ts(HttpServerConfig{});

  ts.postRouterUpdate([this](Router& router) {
    router.setWebSocket("/echo", WebSocketEndpoint::WithFactory([this](const HttpRequest& /*req*/) {
                          auto handler = std::make_unique<WebSocketHandler>();
                          handler->setCallbacks(WebSocketCallbacks{
                              .onMessage =
                                  [this, handler = handler.get()](std::span<const std::byte> payload, bool isBinary) {
                                    _receivedMessages.emplace_back(PayloadToString({payload.begin(), payload.end()}),
                                                                   isBinary);
                                    // Echo back
                                    if (!isBinary) {
                                      handler->sendText(PayloadToString({payload.begin(), payload.end()}));
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
  std::string upgradeResponse = test::recvWithTimeout(conn.fd(), 500ms);
  ASSERT_TRUE(upgradeResponse.contains("HTTP/1.1 101"));

  // Send a text frame
  auto textFrame = BuildClientTextFrame("Hello, WebSocket!");
  test::sendAll(conn.fd(), std::string_view(reinterpret_cast<const char*>(textFrame.data()), textFrame.size()));

  // Wait for echo response
  std::this_thread::sleep_for(50ms);

  // Read response frame
  std::string rawResponse = test::recvWithTimeout(conn.fd(), 500ms);

  // Parse the frame
  std::span<const std::byte> responseData(reinterpret_cast<const std::byte*>(rawResponse.data()), rawResponse.size());
  auto frame = ParseServerFrame(responseData);

  ASSERT_TRUE(frame.has_value()) << "Failed to parse server frame, raw size: " << rawResponse.size();
  EXPECT_EQ(frame.value_or(ServerFrame{}).opcode, Opcode::Text);
  EXPECT_TRUE(frame.value_or(ServerFrame{}).fin);
  EXPECT_EQ(PayloadToString(frame.value_or(ServerFrame{}).payload), "Hello, WebSocket!");

  // Verify server received our message
  ASSERT_EQ(_receivedMessages.size(), 1);
  EXPECT_EQ(_receivedMessages[0].first, "Hello, WebSocket!");
  EXPECT_FALSE(_receivedMessages[0].second);  // Text, not binary
}

TEST_F(WebSocketTest, CloseHandshake) {
  test::TestServer ts(HttpServerConfig{});

  ts.postRouterUpdate([this](Router& router) {
    router.setWebSocket("/ws", WebSocketEndpoint::WithCallbacks(WebSocketCallbacks{
                                   .onMessage = {},
                                   .onPing = {},
                                   .onPong = {},
                                   .onClose =
                                       [this](CloseCode code, std::string_view reason) {
                                         _closeReceived = true;
                                         _closeCode = code;
                                         _closeReason = std::string(reason);
                                       },
                                   .onError = {},
                               }));
  });

  test::ClientConnection conn(ts.port());

  // Upgrade
  test::sendAll(conn.fd(), BuildUpgradeRequest("/ws"));
  std::string upgradeResponse = test::recvWithTimeout(conn.fd(), 500ms);
  ASSERT_TRUE(upgradeResponse.find("HTTP/1.1 101") != std::string::npos);

  // Send close frame
  auto closeFrame = BuildClientCloseFrame(CloseCode::Normal, "goodbye");
  test::sendAll(conn.fd(), std::string_view(reinterpret_cast<const char*>(closeFrame.data()), closeFrame.size()));

  // Wait for close response
  std::this_thread::sleep_for(50ms);

  // Read response
  std::string rawResponse = test::recvWithTimeout(conn.fd(), 500ms);
  std::span<const std::byte> responseData(reinterpret_cast<const std::byte*>(rawResponse.data()), rawResponse.size());
  auto frame = ParseServerFrame(responseData);

  // Server should send close frame back
  ASSERT_TRUE(frame.has_value()) << "Failed to parse close response";
  EXPECT_EQ(frame.value_or(ServerFrame{}).opcode, Opcode::Close);

  // Verify callback was invoked
  EXPECT_TRUE(_closeReceived);
  EXPECT_EQ(_closeCode, CloseCode::Normal);
  EXPECT_EQ(_closeReason, "goodbye");
}

TEST_F(WebSocketTest, WithConfigAndCallbacksCustomMaxMessageSize) {
  test::TestServer ts(HttpServerConfig{});

  WebSocketConfig config;
  config.maxMessageSize = 100;  // Small limit for testing

  ts.postRouterUpdate([this, config](Router& router) {
    router.setWebSocket("/ws", WebSocketEndpoint::WithConfigAndCallbacks(
                                   config, WebSocketCallbacks{
                                               .onMessage =
                                                   [this](std::span<const std::byte> payload, bool isBinary) {
                                                     _receivedMessages.emplace_back(
                                                         PayloadToString({payload.begin(), payload.end()}), isBinary);
                                                   },
                                               .onPing = {},
                                               .onPong = {},
                                               .onClose = {},
                                               .onError = {},
                                           }));
  });

  test::ClientConnection conn(ts.port());

  // Upgrade
  test::sendAll(conn.fd(), BuildUpgradeRequest("/ws"));
  std::string upgradeResponse = test::recvWithTimeout(conn.fd(), 500ms);
  ASSERT_TRUE(upgradeResponse.contains("HTTP/1.1 101"));

  // Send a small message (should work)
  auto smallFrame = BuildClientTextFrame("Small message");
  test::sendAll(conn.fd(), std::string_view(reinterpret_cast<const char*>(smallFrame.data()), smallFrame.size()));

  // Wait for processing
  std::this_thread::sleep_for(50ms);

  EXPECT_EQ(_receivedMessages.size(), 1);
  EXPECT_EQ(_receivedMessages[0].first, "Small message");
}

}  // namespace
