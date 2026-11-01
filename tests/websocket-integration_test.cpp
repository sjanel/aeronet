#include <gtest/gtest.h>

#ifndef AERONET_WINDOWS
#include <sys/socket.h>
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
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
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/router.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/websocket-config.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-deflate-context.hpp"
#include "aeronet/websocket-deflate.hpp"
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
  constexpr std::byte maskKey[]{std::byte{0x37}, std::byte{0xfa}, std::byte{0x21}, std::byte{0x3d}};
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
  constexpr std::byte maskKey[]{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
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
  while (!ParseServerFrame(std::span<const std::byte>(reinterpret_cast<const std::byte*>(raw.data()), raw.size()))) {
    if (test::RecvAppend(fd, raw, 16384) <= 0) {
      return {};
    }
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

// An upgrade request announcing a body, whose client waits for 100 Continue: the upgrade, like a handler, takes place
// once the body is received, so 100 Continue is sent (and not the 405 of a path without handler for GET).
TEST_F(WebSocketTest, UpgradeWaitingFor100ContinueIsUpgradedAfterTheBody) {
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
  test::sendAll(conn.fd(), BuildUpgradeRequestWithHeaders("/ws", "Content-Length: 5\r\nExpect: 100-continue\r\n"));
  EXPECT_EQ(test::RecvHead(conn.fd()), http::HTTP11_100_CONTINUE);
  test::sendAll(conn.fd(), "hello");
  const std::string response = test::RecvHead(conn.fd());
  EXPECT_TRUE(response.starts_with("HTTP/1.1 101")) << "Response: " << response;
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

// An upgrade request pipelined behind a served request, from the same input, followed by a frame sent without waiting
// for the 101 response: the frame is the first input of the WebSocket connection, delivered without waiting for more.
TEST_F(WebSocketTest, PipelinedUpgradeWithCoalescedFrame) {
  ts.postRouterUpdate([this](Router& router) {
    router.setPath(http::Method::GET, "/plain",
                   [](const HttpRequestView&) { return HttpResponse(http::StatusCodeNoContent); });
    router.setWebSocket("/ws", WebSocketEndpoint::WithCallbacks(WebSocketCallbacks{
                                   .onMessage = [this](std::span<const std::byte> payload,
                                                       bool isBinary) { recordMessage(payload, isBinary); },
                                   .onPing = {},
                                   .onPong = {},
                                   .onClose = {},
                                   .onError = {},
                               }));
  });

  test::ClientConnection conn(ts.port());
  const auto textFrame = BuildClientTextFrame("coalesced");
  test::sendAll(conn.fd(), "GET /plain HTTP/1.1\r\nHost: localhost\r\n\r\n" + BuildUpgradeRequest("/ws") +
                               std::string(reinterpret_cast<const char*>(textFrame.data()), textFrame.size()));

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (receivedMessages().empty() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  const auto messages = receivedMessages();
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages[0].first, "coalesced");
  std::string responses;
  while (!(responses.contains("HTTP/1.1 101") && responses.ends_with(http::DoubleCRLF)) &&
         std::chrono::steady_clock::now() < deadline) {
    responses += test::recvWithTimeout(conn.fd(), 10ms);
  }
  EXPECT_TRUE(responses.starts_with("HTTP/1.1 204")) << responses;
  EXPECT_TRUE(responses.contains("HTTP/1.1 101")) << responses;
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
  constexpr std::byte maskKey[]{std::byte{0x5a}, std::byte{0x1c}, std::byte{0xe3}, std::byte{0x07}};
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
  ZSetInput(deflater, text.data(), static_cast<uint32_t>(text.size()));
  ZSetOutput(deflater, compressed.data(), static_cast<uint32_t>(compressed.size()));
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
  ZSetInput(inflater, echoPayload.data(), static_cast<uint32_t>(echoPayload.size()));
  ZSetOutput(inflater, decompressed.data(), static_cast<uint32_t>(decompressed.size()));
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
  auto val = echo.value_or(ServerFrame{});
  EXPECT_EQ(val.opcode, Opcode::Text);
  EXPECT_EQ(PayloadToString(val.payload), "still there?");
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
  auto val = ping.value_or(ServerFrame{});
  EXPECT_EQ(val.opcode, Opcode::Ping);
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
    auto val = ping.value_or(ServerFrame{});
    ASSERT_EQ(val.opcode, Opcode::Ping);
    SendFrame(conn.fd(), BuildClientFrame(Opcode::Pong, PayloadToString(val.payload)));
  }
  SendFrame(conn.fd(), BuildClientTextFrame("alive"));
  auto frame = ReceiveServerFrame(conn.fd(), 1000ms);
  auto val = frame.value_or(ServerFrame{});
  // A Ping may be sent before the echo.
  if (frame.has_value() && val.opcode == Opcode::Ping) {
    frame = ReceiveServerFrame(conn.fd(), 1000ms);
  }
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(val.opcode, Opcode::Text);
  EXPECT_EQ(PayloadToString(val.payload), "alive");
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
  auto val = close.value_or(ServerFrame{});
  EXPECT_EQ(val.opcode, Opcode::Close);
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
  return test::RecvHead(conn.fd());
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

// RFC 9110 §7.8: the Upgrade header of an HTTP/1.0 request is ignored, and an HTTP/1.0 client cannot receive the 101
// (§15.2): the request is served like any GET.
TEST_F(WebSocketTest, UpgradeIgnoredInHttp10) {
  ts.postRouterUpdate([](Router& router) {
    router.setWebSocket("/ws-http10", SilentEndpoint());
    router.setPath(http::Method::GET, "/ws-http10-get", [](const HttpRequestView&) { return HttpResponse("plain"); });
    router.setWebSocket("/ws-http10-get", SilentEndpoint());
  });
  const auto http10Upgrade = [](std::string_view path) {
    std::string req = BuildUpgradeRequest(path);
    req.replace(req.find("HTTP/1.1"), 8, "HTTP/1.0");
    return test::sendAndCollect(ts.port(), req);
  };

  std::string resp = http10Upgrade("/ws-http10");
  EXPECT_TRUE(resp.starts_with("HTTP/1.0 404")) << resp;
  resp = http10Upgrade("/ws-http10-get");
  EXPECT_TRUE(resp.starts_with("HTTP/1.0 200")) << resp;
  EXPECT_TRUE(resp.ends_with("\r\n\r\nplain")) << resp;

  EXPECT_TRUE(UpgradeResponse("/ws-http10", "").starts_with("HTTP/1.1 101"));
  EXPECT_TRUE(UpgradeResponse("/ws-http10-get", "").starts_with("HTTP/1.1 101"));
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
                          throw 42;  // NOLINT(bugprone-std-exception-baseclass)
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
  auto val = echo.value_or(ServerFrame{});
  ASSERT_TRUE(echo.has_value());
  EXPECT_EQ(PayloadToString(val.payload), "ping");
}

// ============================================================================
// Large echoes sent from the input buffer, and compressed connections sharing the streams of a server thread
// ============================================================================

// Endpoint echoing each message with the payload given to onMessage: the large ones are sent from the input buffer.
WebSocketEndpoint EchoFromInputEndpoint(WebSocketConfig config = {}) {
  return WebSocketEndpoint::WithFactory([config](const HttpRequestView& /*req*/) {
    auto handler = std::make_unique<WebSocketHandler>(config);
    handler->setCallbacks(WebSocketCallbacks{
        .onMessage =
            [handler = handler.get()](std::span<const std::byte> payload, bool isBinary) {
              if (isBinary) {
                handler->sendBinary(payload);
              } else {
                handler->sendText(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
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

// Reads the frames sent by the server one after the other (a read may return several of them).
class ServerFrameReader {
 public:
  struct Frame {
    ServerFrame frame;
    bool rsv1{false};
  };

  explicit ServerFrameReader(NativeHandle fd) : _fd(fd) {}

  // Next frame, or nullopt on timeout or close.
  std::optional<Frame> next(std::chrono::milliseconds timeout) {
    test::setRecvTimeout(_fd, timeout);
    for (;;) {
      auto frame =
          ParseServerFrame(std::span<const std::byte>(reinterpret_cast<const std::byte*>(_buf.data()), _buf.size()));
      if (frame) {
        const std::size_t payloadSize = frame->payload.size();
        // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator)
        const std::size_t headerSize = payloadSize < 126 ? 2 : (payloadSize < 65536 ? 4 : 10);
        const bool rsv1 = (static_cast<uint8_t>(_buf[0]) & 0x40U) != 0;
        _buf.erase(0, headerSize + payloadSize);
        return Frame{std::move(*frame), rsv1};
      }
      if (test::RecvAppend(_fd, _buf, 16384) <= 0) {
        return std::nullopt;
      }
    }
  }

 private:
  NativeHandle _fd;
  std::string _buf;
};

std::string MakeBinaryPayload(std::size_t size, std::size_t seed) {
  std::string payload(size, '\0');
  for (std::size_t idx = 0; idx < size; ++idx) {
    payload[idx] = static_cast<char>(((idx * 31U) + (seed * 7U) + (idx >> 9U)) & 0xFFU);
  }
  return payload;
}

std::string_view FrameView(const vector<std::byte>& frame) {
  return {reinterpret_cast<const char*>(frame.data()), frame.size()};
}

TEST_F(WebSocketTest, LargeEchoesSentFromInputBufferAreIntact) {
  ts.postRouterUpdate([](Router& router) { router.setWebSocket("/echo-input", EchoFromInputEndpoint()); });

  test::ClientConnection conn(ts.port());
  Upgrade(conn.fd(), "/echo-input");
  ServerFrameReader reader(conn.fd());
  std::size_t seed = 0;
  for (const std::size_t size :
       {std::size_t{4095}, std::size_t{4096}, std::size_t{65536}, std::size_t{300000}, std::size_t{1} << 20U}) {
    for (int round = 0; round < 2; ++round) {
      const std::string payload = MakeBinaryPayload(size, ++seed);
      test::sendAll(conn.fd(), FrameView(BuildClientFrame(Opcode::Binary, payload)), 5000ms);
      const auto optEcho = reader.next(5000ms);
      ASSERT_TRUE(optEcho.has_value()) << size;

      auto echo = optEcho.value_or(ServerFrameReader::Frame{});

      EXPECT_EQ(echo.frame.opcode, Opcode::Binary);
      EXPECT_TRUE(PayloadToString(echo.frame.payload) == payload) << size << " round " << round;
    }
  }
}

// The client sends large messages without reading the echoes: the output of the server blocks, so the unsent part of
// each echo sent from the input buffer is copied before this input is consumed.
TEST_F(WebSocketTest, LargeEchoesBlockedByThePeerAreIntact) {
  ts.postRouterUpdate([](Router& router) { router.setWebSocket("/echo-input-blocked", EchoFromInputEndpoint()); });

  test::ClientConnection conn(ts.port());
  Upgrade(conn.fd(), "/echo-input-blocked");

  constexpr uint32_t kNbMessages = 24;
  constexpr std::size_t kSize = 512UL * 1024UL;
  vector<std::string> payloads;
  std::string frames;
  for (uint32_t idx = 0; idx < kNbMessages; ++idx) {
    payloads.push_back(MakeBinaryPayload(kSize, idx));
    frames.append(FrameView(BuildClientFrame(Opcode::Binary, payloads.back())));
  }

  // The server stops reading once its output blocks: the sender waits until the echoes are read. Joined on a failed
  // assertion too (by its destructor), instead of terminating the test binary.
  bool sendFailed = false;
  std::jthread sender([&] {
    try {
      test::sendAll(conn.fd(), frames, 20000ms);
    } catch (const std::exception&) {
      sendFailed = true;
    }
  });
  std::this_thread::sleep_for(200ms);

  ServerFrameReader reader(conn.fd());
  for (uint32_t idx = 0; idx < kNbMessages; ++idx) {
    const auto optEcho = reader.next(10000ms);
    ASSERT_TRUE(optEcho.has_value()) << idx;
    auto echo = optEcho.value_or(ServerFrameReader::Frame{});
    EXPECT_TRUE(PayloadToString(echo.frame.payload) == payloads[idx]) << idx;
  }
  sender.join();
  EXPECT_FALSE(sendFailed);
}

#ifdef AERONET_ENABLE_ZLIB

// Connections of a server thread negotiating permessage-deflate: those without context takeover share the streams of
// the thread, the other one keeps its own, its messages referring to the previous ones. The interleaved messages of all
// of them must decompress, each way.
TEST_F(WebSocketTest, CompressedConnectionsOfAServerThread) {
  ts.postRouterUpdate([](Router& router) {
    WebSocketConfig config;
    config.deflateConfig.minCompressSize = 16;
    auto endpoint = EchoFromInputEndpoint(config);
    endpoint.config = config;
    router.setWebSocket("/echo-deflate-shared", std::move(endpoint));
  });

  const DeflateNegotiatedParams noContextTakeover{
      .serverMaxWindowBits = 15,
      .clientMaxWindowBits = 15,
      .serverNoContextTakeover = true,
      .clientNoContextTakeover = true,
  };
  struct Client {
    std::string_view extension;
    DeflateNegotiatedParams params{.serverMaxWindowBits = 15};
  };
  const Client clients[]{
      {"permessage-deflate; client_no_context_takeover; server_no_context_takeover", noContextTakeover},
      {"permessage-deflate", DeflateNegotiatedParams{.serverMaxWindowBits = 15}},
      {"permessage-deflate; server_no_context_takeover; client_no_context_takeover", noContextTakeover},
  };
  constexpr uint32_t kNbClients = std::size(clients);

  test::ClientConnection conns[kNbClients]{
      test::ClientConnection(ts.port()),
      test::ClientConnection(ts.port()),
      test::ClientConnection(ts.port()),
  };
  vector<std::unique_ptr<DeflateContext>> codecs;
  vector<std::unique_ptr<ServerFrameReader>> readers;
  for (uint32_t idx = 0; idx < kNbClients; ++idx) {
    test::sendAll(conns[idx].fd(),
                  BuildUpgradeRequest("/echo-deflate-shared", "dGhlIHNhbXBsZSBub25jZQ==",
                                      "Sec-WebSocket-Extensions: " + std::string(clients[idx].extension) + "\r\n"));
    const std::string head = ReceiveResponseHead(conns[idx].fd(), 1000ms);
    ASSERT_TRUE(head.starts_with("HTTP/1.1 101")) << head;
    ASSERT_TRUE(head.contains("permessage-deflate")) << head;
    codecs.push_back(std::make_unique<DeflateContext>(clients[idx].params, DeflateConfig(), /*isServerSide=*/false));
    readers.push_back(std::make_unique<ServerFrameReader>(conns[idx].fd()));
  }

  for (uint32_t round = 0; round < 4; ++round) {
    for (uint32_t idx = 0; idx < kNbClients; ++idx) {
      std::string text;
      const uint32_t nbItems = 3 + (((round * kNbClients) + idx) * 97U % 400U);
      for (uint32_t item = 0; item < nbItems; ++item) {
        text += R"({"client":)" + std::to_string(idx) + R"(,"round":)" + std::to_string(round) + R"(,"item":)" +
                std::to_string(item) + "}";
      }
      RawBytes compressed;
      ASSERT_EQ(codecs[idx]->compress(std::as_bytes(std::span<const char>(text.data(), text.size())), compressed),
                nullptr);
      const auto frame = BuildClientFrame(
          std::string_view(reinterpret_cast<const char*>(compressed.data()), compressed.size()), Opcode::Text, true);
      test::sendAll(conns[idx].fd(), FrameView(frame), 2000ms);

      const auto optEcho = readers[idx]->next(2000ms);
      ASSERT_TRUE(optEcho.has_value()) << "client " << idx << " round " << round;

      auto echo = optEcho.value_or(ServerFrameReader::Frame{});

      EXPECT_TRUE(echo.rsv1);
      RawBytes decompressed;
      ASSERT_EQ(codecs[idx]->decompress(
                    std::span<const std::byte>(echo.frame.payload.data(), echo.frame.payload.size()), decompressed),
                nullptr)
          << "client " << idx << " round " << round;
      EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(decompressed.data()), decompressed.size()), text)
          << "client " << idx << " round " << round;
    }
  }
}

#endif

}  // namespace
