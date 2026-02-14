#include "aeronet/websocket-handler.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-deflate.hpp"
#include "aeronet/websocket-frame.hpp"

namespace aeronet::websocket {
namespace {

std::span<const std::byte> sv_bytes(std::string_view sv) noexcept {
  return std::as_bytes(std::span<const char>(sv.data(), sv.size()));
}

std::span<const std::byte> buf_bytes(const RawBytes& buf) noexcept { return {buf.data(), buf.size()}; }

template <typename Container>
std::span<const std::byte> container_bytes(const Container& cont) noexcept {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(cont.data()), cont.size());
}

}  // namespace

class WebSocketHandlerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create handler with callbacks that record what happened
    WebSocketCallbacks callbacks;
    callbacks.onMessage = [this](std::span<const std::byte> payload, bool isBinary) {
      lastMessageBinary = isBinary;
      lastMessage = std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
      messageCount++;
    };
    callbacks.onPing = [this](std::span<const std::byte> payload) {
      lastPingPayload = std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
      pingCount++;
    };
    callbacks.onPong = [this](std::span<const std::byte> payload) {
      lastPongPayload = std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
      pongCount++;
    };
    callbacks.onClose = [this](CloseCode code, std::string_view reason) {
      lastCloseCode = code;
      lastCloseReason = std::string(reason);
      closeCount++;
    };
    callbacks.onError = [this](CloseCode code, std::string_view message) {
      lastErrorCode = code;
      lastErrorMessage = std::string(message);
      errorCount++;
    };

    // Default to server-side configuration (clients must mask)
    WebSocketConfig config;
    config.isServerSide = false;  // Accept unmasked frames for easier testing
    handler = std::make_unique<WebSocketHandler>(config, std::move(callbacks));
  }

  // Helper to build a masked frame (simulating client->server)
  static RawBytes BuildMaskedFrame(Opcode opcode, std::string_view payload, bool fin = true) {
    RawBytes frame;
    MaskingKey mask = MakeMask(0x12, 0x34, 0x56, 0x78);
    BuildFrame(frame, opcode, sv_bytes(payload), fin, true, mask);
    return frame;
  }

  // Helper to construct a MaskingKey from four bytes (matching previous
  // {std::byte, std::byte, std::byte, std::byte} ordering).
  static MaskingKey MakeMask(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) noexcept {
    return static_cast<MaskingKey>(b0) | (static_cast<MaskingKey>(b1) << 8) | (static_cast<MaskingKey>(b2) << 16) |
           (static_cast<MaskingKey>(b3) << 24);
  }

  // Helper to build an unmasked frame (simulating server->client)
  static RawBytes BuildUnmaskedFrame(Opcode opcode, std::string_view payload, bool fin = true) {
    RawBytes frame;
    BuildFrame(frame, opcode, sv_bytes(payload), fin, false);
    return frame;
  }

  // Helper to process frame data
  ProtocolProcessResult process(const RawBytes& data) { return handler->processInput(buf_bytes(data), dummyState); }

  std::unique_ptr<WebSocketHandler> handler;
  ConnectionState dummyState;  // Not used in current implementation

  // Tracking variables for callbacks
  std::string lastMessage;
  bool lastMessageBinary{false};
  int messageCount{0};

  std::string lastPingPayload;
  int pingCount{0};

  std::string lastPongPayload;
  int pongCount{0};

  CloseCode lastCloseCode{CloseCode::Normal};
  std::string lastCloseReason;
  int closeCount{0};

  CloseCode lastErrorCode{CloseCode::Normal};
  std::string lastErrorMessage;
  int errorCount{0};
};

// ============================================================================
// Basic message tests
// ============================================================================

TEST_F(WebSocketHandlerTest, ReceiveTextMessage) {
  auto frame = BuildUnmaskedFrame(Opcode::Text, "Hello, World!");
  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Continue);
  EXPECT_EQ(messageCount, 1);
  EXPECT_EQ(lastMessage, "Hello, World!");
  EXPECT_FALSE(lastMessageBinary);
}

TEST_F(WebSocketHandlerTest, ReceiveBinaryMessage) {
  std::array<uint8_t, 4> binaryData = {0xDE, 0xAD, 0xBE, 0xEF};
  RawBytes frame;
  BuildFrame(frame, Opcode::Binary, container_bytes(binaryData), true, false);

  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Continue);
  EXPECT_EQ(messageCount, 1);
  EXPECT_TRUE(lastMessageBinary);
  EXPECT_EQ(lastMessage.size(), 4);
}

TEST_F(WebSocketHandlerTest, ReceiveEmptyMessage) {
  auto frame = BuildUnmaskedFrame(Opcode::Text, "");
  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Continue);
  EXPECT_EQ(messageCount, 1);
  EXPECT_EQ(lastMessage, "");
}

TEST_F(WebSocketHandlerTest, ReceiveMultipleMessages) {
  auto frame1 = BuildUnmaskedFrame(Opcode::Text, "First");
  auto frame2 = BuildUnmaskedFrame(Opcode::Text, "Second");

  // Concatenate frames
  RawBytes combined;
  combined.append(frame1.data(), frame1.size());
  combined.append(frame2.data(), frame2.size());

  (void)process(combined);

  EXPECT_EQ(messageCount, 2);
  EXPECT_EQ(lastMessage, "Second");
}

// ============================================================================
// Fragmentation tests
// ============================================================================

TEST_F(WebSocketHandlerTest, ReceiveFragmentedMessage) {
  // First fragment: opcode=Text, FIN=0
  auto frag1 = BuildUnmaskedFrame(Opcode::Text, "Hello, ", false);
  // Continuation: opcode=Continuation, FIN=0
  auto frag2 = BuildUnmaskedFrame(Opcode::Continuation, "World", false);
  // Final fragment: opcode=Continuation, FIN=1
  auto frag3 = BuildUnmaskedFrame(Opcode::Continuation, "!", true);

  process(frag1);
  EXPECT_EQ(messageCount, 0);  // Not complete yet

  process(frag2);
  EXPECT_EQ(messageCount, 0);  // Still not complete

  (void)process(frag3);
  EXPECT_EQ(messageCount, 1);
  EXPECT_EQ(lastMessage, "Hello, World!");
}

TEST_F(WebSocketHandlerTest, UnexpectedContinuationFrame) {
  // Continuation frame without a preceding data frame
  auto frame = BuildUnmaskedFrame(Opcode::Continuation, "data", true);
  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(errorCount, 1);
  EXPECT_EQ(lastErrorCode, CloseCode::ProtocolError);
}

TEST_F(WebSocketHandlerTest, NewMessageDuringFragment) {
  // Start a fragmented message
  auto frag1 = BuildUnmaskedFrame(Opcode::Text, "Start", false);
  process(frag1);

  // Try to start a new message before completing
  auto newMsg = BuildUnmaskedFrame(Opcode::Binary, "New", true);
  auto result = process(newMsg);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(errorCount, 1);
  EXPECT_EQ(lastErrorCode, CloseCode::ProtocolError);
}

// ============================================================================
// Control frame tests
// ============================================================================

TEST_F(WebSocketHandlerTest, ReceivePing) {
  RawBytes frame;
  BuildCloseFrame(frame, CloseCode::Normal, "");  // Just to get structure
  frame.clear();
  BuildFrame(frame, Opcode::Ping, sv_bytes(std::string_view("ping data", 9)), true, false);

  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::ResponseReady);
  EXPECT_EQ(pingCount, 1);
  EXPECT_EQ(lastPingPayload, "ping data");

  // Handler should have queued a Pong response
  EXPECT_TRUE(handler->hasPendingOutput());
}

TEST_F(WebSocketHandlerTest, ReceivePong) {
  RawBytes frame;
  BuildFrame(frame, Opcode::Pong, sv_bytes(std::string_view("pong data", 9)), true, false);

  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Continue);
  EXPECT_EQ(pongCount, 1);
  EXPECT_EQ(lastPongPayload, "pong data");
}

TEST_F(WebSocketHandlerTest, ReceiveClose) {
  RawBytes frame;
  BuildCloseFrame(frame, CloseCode::Normal, "Goodbye");

  (void)process(frame);

  EXPECT_EQ(closeCount, 1);
  EXPECT_EQ(lastCloseCode, CloseCode::Normal);
  EXPECT_EQ(lastCloseReason, "Goodbye");

  // Handler should have queued a Close response
  EXPECT_TRUE(handler->hasPendingOutput());
  EXPECT_TRUE(handler->isClosing());
}

TEST_F(WebSocketHandlerTest, ControlFrameDuringFragment) {
  // Control frames can be interleaved during fragmented messages
  auto frag1 = BuildUnmaskedFrame(Opcode::Text, "Part1", false);
  process(frag1);

  // Receive a ping during fragmentation
  RawBytes pingFrame;
  BuildFrame(pingFrame, Opcode::Ping, sv_bytes(std::string_view("ping", 4)), true, false);
  auto pingResult = process(pingFrame);

  EXPECT_EQ(pingResult.action, ProtocolProcessResult::Action::ResponseReady);
  EXPECT_EQ(pingCount, 1);

  // Continue with fragment
  auto frag2 = BuildUnmaskedFrame(Opcode::Continuation, "Part2", true);
  process(frag2);

  EXPECT_EQ(messageCount, 1);
  EXPECT_EQ(lastMessage, "Part1Part2");
}

// ============================================================================
// UTF-8 validation tests
// ============================================================================

TEST_F(WebSocketHandlerTest, ValidUtf8Text) {
  // Valid UTF-8 with multi-byte characters
  auto frame = BuildUnmaskedFrame(Opcode::Text, "Hello 世界 🌍");
  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Continue);
  EXPECT_EQ(messageCount, 1);
  EXPECT_EQ(lastMessage, "Hello 世界 🌍");
}

TEST_F(WebSocketHandlerTest, InvalidUtf8Text) {
  // Invalid UTF-8 sequence
  std::array<char, 4> invalidUtf8 = {'\xC0', '\x80', 'a', 'b'};  // Overlong encoding
  RawBytes frame;
  BuildFrame(frame, Opcode::Text, container_bytes(invalidUtf8), true, false);

  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(errorCount, 1);
  EXPECT_EQ(lastErrorCode, CloseCode::InvalidPayloadData);
}

TEST_F(WebSocketHandlerTest, Utf8SurrogatePairInvalid) {
  // UTF-16 surrogate pair encoded in UTF-8 (invalid)
  std::array<char, 3> surrogate = {'\xED', '\xA0', '\x80'};  // U+D800 surrogate
  RawBytes frame;
  BuildFrame(frame, Opcode::Text, container_bytes(surrogate), true, false);

  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(lastErrorCode, CloseCode::InvalidPayloadData);
}

// ============================================================================
// Send API tests
// ============================================================================

TEST_F(WebSocketHandlerTest, SendText) {
  EXPECT_TRUE(handler->sendText("Hello"));
  EXPECT_TRUE(handler->hasPendingOutput());

  auto output = handler->getPendingOutput();
  EXPECT_FALSE(output.empty());
}

TEST_F(WebSocketHandlerTest, SendBinary) {
  std::array<std::byte, 3> data = {std::byte{1}, std::byte{2}, std::byte{3}};
  EXPECT_TRUE(handler->sendBinary(data));
  EXPECT_TRUE(handler->hasPendingOutput());
}

TEST_F(WebSocketHandlerTest, SendPing) {
  std::array<std::byte, 4> payload = {std::byte{'p'}, std::byte{'i'}, std::byte{'n'}, std::byte{'g'}};
  EXPECT_TRUE(handler->sendPing(payload));
  EXPECT_TRUE(handler->hasPendingOutput());
}

TEST_F(WebSocketHandlerTest, SendClose) {
  EXPECT_TRUE(handler->sendClose(CloseCode::Normal, "Goodbye"));
  EXPECT_TRUE(handler->isClosing());
  EXPECT_TRUE(handler->hasPendingOutput());

  // Second close should fail
  EXPECT_FALSE(handler->sendClose(CloseCode::Normal, "Again"));
}

TEST_F(WebSocketHandlerTest, SendAfterClose) {
  handler->sendClose(CloseCode::Normal, "Bye");

  // Sending messages after close should fail
  EXPECT_FALSE(handler->sendText("Hello"));
  EXPECT_FALSE(handler->sendBinary({}));
  EXPECT_FALSE(handler->sendPing({}));
}

// ============================================================================
// Output management tests
// ============================================================================

TEST_F(WebSocketHandlerTest, OutputWrittenPartially) {
  handler->sendText("Test message");

  auto output = handler->getPendingOutput();
  std::size_t totalSize = output.size();

  // Simulate partial write
  handler->onOutputWritten(5);

  // Should still have pending output
  EXPECT_TRUE(handler->hasPendingOutput());
  auto remaining = handler->getPendingOutput();
  EXPECT_EQ(remaining.size(), totalSize - 5);
}

TEST_F(WebSocketHandlerTest, OutputWrittenFully) {
  handler->sendText("Test");

  auto output = handler->getPendingOutput();
  handler->onOutputWritten(output.size());

  EXPECT_FALSE(handler->hasPendingOutput());
}

// ============================================================================
// Close handshake tests
// ============================================================================

TEST_F(WebSocketHandlerTest, CloseHandshakeInitiatedByUs) {
  handler->sendClose(CloseCode::GoingAway, "Bye");
  EXPECT_TRUE(handler->isClosing());
  EXPECT_FALSE(handler->isCloseComplete());

  // Receive close response
  RawBytes closeFrame;
  BuildCloseFrame(closeFrame, CloseCode::GoingAway, "Bye");
  auto result = process(closeFrame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_TRUE(handler->isCloseComplete());
}

TEST_F(WebSocketHandlerTest, CloseHandshakeInitiatedByPeer) {
  RawBytes closeFrame;
  BuildCloseFrame(closeFrame, CloseCode::Normal, "Peer closing");
  (void)process(closeFrame);

  EXPECT_TRUE(handler->isClosing());
  EXPECT_TRUE(handler->isCloseComplete());
  EXPECT_TRUE(handler->hasPendingOutput());  // Should echo Close
}

// ============================================================================
// Message size limit tests
// ============================================================================

TEST_F(WebSocketHandlerTest, MessageTooLarge) {
  WebSocketConfig config;
  config.isServerSide = false;
  config.maxMessageSize = 100;  // Small limit
  handler = std::make_unique<WebSocketHandler>(config);

  // Try to send a large message via fragments
  std::string largePayload(60, 'X');
  auto frag1 = BuildUnmaskedFrame(Opcode::Text, largePayload, false);
  process(frag1);

  // Second fragment exceeds limit
  auto frag2 = BuildUnmaskedFrame(Opcode::Continuation, largePayload, true);
  auto result = process(frag2);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
}

TEST_F(WebSocketHandlerTest, MessageTooLargeTriggersOnError) {
  WebSocketConfig config;
  config.isServerSide = false;
  config.maxMessageSize = 100;  // Small limit

  // Capture errors via callbacks
  WebSocketCallbacks callbacks;
  callbacks.onError = [this](CloseCode code, std::string_view message) {
    lastErrorCode = code;
    lastErrorMessage = std::string(message);
    errorCount++;
  };

  auto limitedHandler = std::make_unique<WebSocketHandler>(config, std::move(callbacks));

  // Send fragmented message that exceeds maxMessageSize
  std::string largePayload(60, 'X');
  auto frag1 = BuildUnmaskedFrame(Opcode::Text, largePayload, false);
  (void)limitedHandler->processInput(buf_bytes(frag1), dummyState);

  auto frag2 = BuildUnmaskedFrame(Opcode::Continuation, largePayload, true);
  auto result = limitedHandler->processInput(buf_bytes(frag2), dummyState);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(errorCount, 1);
  EXPECT_EQ(lastErrorCode, CloseCode::MessageTooBig);
  EXPECT_EQ(lastErrorMessage, "Message too large");
}

// ============================================================================
// Incomplete frame handling tests
// ============================================================================

TEST_F(WebSocketHandlerTest, IncompleteFrame) {
  auto frame = BuildUnmaskedFrame(Opcode::Text, "Complete message");

  // Only send half the frame
  RawBytes partial;
  partial.append(frame.data(), frame.size() / 2);

  auto result = process(partial);
  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Continue);
  EXPECT_EQ(messageCount, 0);

  // Send the rest
  RawBytes rest;
  rest.append(frame.data() + (frame.size() / 2), frame.size() - (frame.size() / 2));
  result = process(rest);

  EXPECT_EQ(messageCount, 1);
  EXPECT_EQ(lastMessage, "Complete message");
}

// ============================================================================
// Factory function tests
// ============================================================================

TEST_F(WebSocketHandlerTest, CreateServerHandler) {
  auto serverHandler = CreateServerWebSocketHandler();
  EXPECT_EQ(serverHandler->type(), ProtocolType::WebSocket);
  EXPECT_TRUE(serverHandler->config().isServerSide);
}

TEST_F(WebSocketHandlerTest, CreateClientHandler) {
  auto clientHandler = CreateClientWebSocketHandler();
  EXPECT_EQ(clientHandler->type(), ProtocolType::WebSocket);
  EXPECT_FALSE(clientHandler->config().isServerSide);
}

// ============================================================================
// Protocol type test
// ============================================================================

TEST_F(WebSocketHandlerTest, ProtocolType) { EXPECT_EQ(handler->type(), ProtocolType::WebSocket); }

// ============================================================================
// Lifecycle tests
// ============================================================================

TEST_F(WebSocketHandlerTest, InitiateClose) {
  handler->initiateClose();
  EXPECT_TRUE(handler->isClosing());
  EXPECT_TRUE(handler->hasPendingOutput());
}

TEST_F(WebSocketHandlerTest, OnTransportClosing) {
  handler->onTransportClosing();
  EXPECT_TRUE(handler->isCloseComplete());
}

// ============================================================================
// Move semantics tests
// ============================================================================

TEST_F(WebSocketHandlerTest, MoveConstruction) {
  handler->sendText("Hello");
  EXPECT_TRUE(handler->hasPendingOutput());

  WebSocketHandler movedHandler(std::move(*handler));

  EXPECT_EQ(movedHandler.type(), ProtocolType::WebSocket);
  EXPECT_TRUE(movedHandler.hasPendingOutput());
}

TEST_F(WebSocketHandlerTest, MoveAssignment) {
  handler->sendText("Hello");
  EXPECT_TRUE(handler->hasPendingOutput());

  WebSocketConfig config;
  config.isServerSide = true;
  WebSocketHandler newHandler(config);
  newHandler = std::move(*handler);

  EXPECT_EQ(newHandler.type(), ProtocolType::WebSocket);
  EXPECT_TRUE(newHandler.hasPendingOutput());
}

// ============================================================================
// Client-side masking tests
// ============================================================================

TEST_F(WebSocketHandlerTest, ClientSideMasksOutgoingFrames) {
  // Create a client-side handler
  WebSocketConfig config;
  config.isServerSide = false;  // Client side - should mask outgoing
  auto clientHandler = std::make_unique<WebSocketHandler>(config);

  clientHandler->sendText("Hello");
  EXPECT_TRUE(clientHandler->hasPendingOutput());

  auto output = clientHandler->getPendingOutput();
  ASSERT_GE(output.size(), 2);

  // Second byte should have mask bit set
  auto byte1 = static_cast<uint8_t>(output[1]);
  EXPECT_TRUE((byte1 & 0x80) != 0);  // MASK bit should be set
}

TEST_F(WebSocketHandlerTest, ServerSideDoesNotMaskOutgoing) {
  // Create server-side handler
  WebSocketConfig config;
  config.isServerSide = true;
  auto serverHandler = std::make_unique<WebSocketHandler>(config);

  serverHandler->sendText("Hello");
  EXPECT_TRUE(serverHandler->hasPendingOutput());

  auto output = serverHandler->getPendingOutput();
  ASSERT_GE(output.size(), 2);

  // Second byte should NOT have mask bit set
  auto byte1 = static_cast<uint8_t>(output[1]);
  EXPECT_FALSE((byte1 & 0x80) != 0);  // MASK bit should NOT be set
}

// ============================================================================
// Server-side masked frame processing tests
// ============================================================================

TEST_F(WebSocketHandlerTest, ServerSideReceivesMaskedFrame) {
  // Create server-side handler
  WebSocketConfig config;
  config.isServerSide = true;
  WebSocketCallbacks callbacks;
  callbacks.onMessage = [this](std::span<const std::byte> payload, bool /*isBinary*/) {
    lastMessage = std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
    messageCount++;
  };
  auto serverHandler = std::make_unique<WebSocketHandler>(config, std::move(callbacks));

  // Build a masked frame (client->server)
  auto maskedFrame = BuildMaskedFrame(Opcode::Text, "Hello");

  auto result = serverHandler->processInput(buf_bytes(maskedFrame), dummyState);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Continue);
  EXPECT_EQ(messageCount, 1);
  EXPECT_EQ(lastMessage, "Hello");
}

// ============================================================================
// Pong during close tests
// ============================================================================

TEST_F(WebSocketHandlerTest, SendPongDuringCloseSent) {
  // Initiate close
  handler->sendClose(CloseCode::Normal, "Closing");
  EXPECT_TRUE(handler->isClosing());

  // Clear output
  auto output = handler->getPendingOutput();
  handler->onOutputWritten(output.size());

  // Should still be able to send pong during close handshake
  std::array<std::byte, 4> payload = {std::byte{'p'}, std::byte{'o'}, std::byte{'n'}, std::byte{'g'}};
  EXPECT_TRUE(handler->sendPong(payload));
  EXPECT_TRUE(handler->hasPendingOutput());
}

TEST_F(WebSocketHandlerTest, SendPongAfterClosed) {
  // Complete close
  handler->onTransportClosing();
  EXPECT_TRUE(handler->isCloseComplete());

  // Should NOT be able to send pong after closed
  std::array<std::byte, 4> payload = {std::byte{'p'}, std::byte{'o'}, std::byte{'n'}, std::byte{'g'}};
  EXPECT_FALSE(handler->sendPong(payload));
}

// ============================================================================
// RSV bits validation tests
// ============================================================================

TEST_F(WebSocketHandlerTest, RSVBitsSetRejectsFrame) {
  // Build a frame with RSV1 bit set (would be for extensions)
  RawBytes frame;
  frame.push_back(std::byte{0x91});  // FIN=1, RSV1=1, opcode=Text
  frame.push_back(std::byte{0x05});  // MASK=0, length=5
  frame.append(reinterpret_cast<const std::byte*>("Hello"), 5);

  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(errorCount, 1);
  EXPECT_EQ(lastErrorCode, CloseCode::ProtocolError);
}

// ============================================================================
// Reserved opcode validation tests
// ============================================================================

TEST_F(WebSocketHandlerTest, ReservedOpcodeRejectsFrame) {
  // Build a frame with reserved opcode (e.g., 3)
  RawBytes frame;
  frame.push_back(std::byte{0x83});  // FIN=1, opcode=3 (reserved)
  frame.push_back(std::byte{0x00});  // MASK=0, length=0

  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(errorCount, 1);
  EXPECT_EQ(lastErrorCode, CloseCode::ProtocolError);
}

// ============================================================================
// Control frame fragmentation validation tests
// ============================================================================

TEST_F(WebSocketHandlerTest, FragmentedPingRejectsFrame) {
  // Build a Ping frame with FIN=0 (invalid - control frames can't be fragmented)
  RawBytes frame;
  frame.push_back(std::byte{0x09});  // FIN=0, opcode=Ping
  frame.push_back(std::byte{0x00});  // MASK=0, length=0

  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(errorCount, 1);
  EXPECT_EQ(lastErrorCode, CloseCode::ProtocolError);
}

// ============================================================================
// Control frame payload too large tests
// ============================================================================

TEST_F(WebSocketHandlerTest, ControlFramePayloadTooLarge) {
  // Build a Ping frame with payload > 125 bytes
  RawBytes frame;
  frame.push_back(std::byte{0x89});  // FIN=1, opcode=Ping
  frame.push_back(std::byte{126});   // MASK=0, 16-bit length indicator
  frame.push_back(std::byte{0x00});
  frame.push_back(std::byte{130});  // 130 bytes > 125
  // Add 130 bytes of payload
  for (int idx = 0; idx < 130; ++idx) {
    frame.push_back(std::byte{'X'});
  }

  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(errorCount, 1);
  EXPECT_EQ(lastErrorCode, CloseCode::ProtocolError);
}

// ============================================================================
// Pong payload truncation tests
// ============================================================================

TEST_F(WebSocketHandlerTest, SendPongTruncatesLongPayload) {
  // Create a server-side handler (no masking = smaller output)
  WebSocketConfig config;
  config.isServerSide = true;
  auto serverHandler = std::make_unique<WebSocketHandler>(config);

  // Send pong with payload > 125 bytes
  std::array<std::byte, 150> longPayload;
  std::ranges::fill(longPayload, std::byte{'X'});

  EXPECT_TRUE(serverHandler->sendPong(longPayload));
  EXPECT_TRUE(serverHandler->hasPendingOutput());

  // Verify the output frame has truncated payload
  auto output = serverHandler->getPendingOutput();
  // Frame should be 2 (header) + 125 (truncated payload) = 127 bytes for unmasked
  EXPECT_EQ(output.size(), 127);
}

// ============================================================================
// Empty getPendingOutput tests
// ============================================================================

TEST_F(WebSocketHandlerTest, GetPendingOutputEmpty) {
  EXPECT_FALSE(handler->hasPendingOutput());
  auto output = handler->getPendingOutput();
  EXPECT_TRUE(output.empty());
}

// ============================================================================
// Unknown control opcode tests
// ============================================================================

TEST_F(WebSocketHandlerTest, UnknownControlOpcode) {
  // Build a frame with control opcode 0x0B (reserved control opcode)
  RawBytes frame;
  frame.push_back(std::byte{0x8B});  // FIN=1, opcode=0x0B
  frame.push_back(std::byte{0x00});  // MASK=0, length=0

  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(errorCount, 1);
  EXPECT_EQ(lastErrorCode, CloseCode::ProtocolError);
}

// ============================================================================
// UTF-8 validation edge cases
// ============================================================================

TEST_F(WebSocketHandlerTest, Utf8IncompleteAtEnd) {
  // UTF-8 sequence starting but incomplete at end of data
  std::array<char, 3> incomplete = {'a', static_cast<char>(0xC2), 'b'};  // 0xC2 starts 2-byte seq
  incomplete[2] = 'b';                                                   // Not a continuation byte
  RawBytes frame;
  BuildFrame(frame, Opcode::Text, container_bytes(incomplete), true, false);

  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(lastErrorCode, CloseCode::InvalidPayloadData);
}

TEST_F(WebSocketHandlerTest, Utf8OutOfRange) {
  // Codepoint > U+10FFFF (4-byte sequence: F4 90 80 80 would be U+110000)
  std::array<char, 4> outOfRange = {'\xF4', '\x90', '\x80', '\x80'};
  RawBytes frame;
  BuildFrame(frame, Opcode::Text, container_bytes(outOfRange), true, false);

  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(lastErrorCode, CloseCode::InvalidPayloadData);
}

TEST_F(WebSocketHandlerTest, Utf8InvalidLeadingByte) {
  // Invalid leading byte (continuation byte without lead)
  std::array<char, 2> invalidLead = {'\x80', 'a'};
  RawBytes frame;
  BuildFrame(frame, Opcode::Text, container_bytes(invalidLead), true, false);

  auto result = process(frame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(lastErrorCode, CloseCode::InvalidPayloadData);
}

// ============================================================================
// Input buffer management tests
// ============================================================================

TEST_F(WebSocketHandlerTest, InputBufferCarryOver) {
  // Build two frames
  auto frame1 = BuildUnmaskedFrame(Opcode::Text, "First");
  auto frame2 = BuildUnmaskedFrame(Opcode::Text, "Second");

  // Send partial first frame, then complete first and start second
  RawBytes partial1;
  partial1.append(frame1.data(), 3);
  auto result1 = process(partial1);
  EXPECT_EQ(result1.action, ProtocolProcessResult::Action::Continue);
  EXPECT_EQ(messageCount, 0);

  // Send rest of first frame + complete second frame
  RawBytes rest;
  rest.append(frame1.data() + 3, frame1.size() - 3);
  rest.append(frame2.data(), frame2.size());
  (void)process(rest);

  EXPECT_EQ(messageCount, 2);
  EXPECT_EQ(lastMessage, "Second");
}

// ============================================================================
// Payload too large tests
// ============================================================================

TEST_F(WebSocketHandlerTest, PayloadTooLargeError) {
  // Create handler with small max frame size
  WebSocketConfig config;
  config.isServerSide = false;
  config.maxFrameSize = 100;
  WebSocketCallbacks callbacks;
  callbacks.onError = [this](CloseCode code, std::string_view /*message*/) {
    lastErrorCode = code;
    errorCount++;
  };
  auto limitedHandler = std::make_unique<WebSocketHandler>(config, std::move(callbacks));

  // Build a frame with header indicating a very large payload
  RawBytes frame;
  frame.push_back(std::byte{0x82});  // FIN=1, opcode=binary
  frame.push_back(std::byte{127});   // 64-bit length indicator
  // Set length to a large value
  frame.push_back(std::byte{0x00});
  frame.push_back(std::byte{0x00});
  frame.push_back(std::byte{0x00});
  frame.push_back(std::byte{0x01});  // 16MB+
  frame.push_back(std::byte{0x00});
  frame.push_back(std::byte{0x00});
  frame.push_back(std::byte{0x00});
  frame.push_back(std::byte{0x00});

  auto result = limitedHandler->processInput(buf_bytes(frame), dummyState);
  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(lastErrorCode, CloseCode::MessageTooBig);
}

// ============================================================================
// Ping truncation tests
// ============================================================================

TEST_F(WebSocketHandlerTest, SendPingTruncatesLongPayload) {
  WebSocketConfig config;
  config.isServerSide = true;
  auto serverHandler = std::make_unique<WebSocketHandler>(config);

  // Send ping with payload > 125 bytes
  std::array<std::byte, 150> longPayload;
  std::ranges::fill(longPayload, std::byte{'P'});

  EXPECT_TRUE(serverHandler->sendPing(longPayload));
  EXPECT_TRUE(serverHandler->hasPendingOutput());

  auto output = serverHandler->getPendingOutput();
  // Frame should be 2 (header) + 125 (truncated payload) = 127 bytes
  EXPECT_EQ(output.size(), 127);
}

// ============================================================================
// SetCallbacks tests
// ============================================================================

TEST_F(WebSocketHandlerTest, SetCallbacksAfterConstruction) {
  WebSocketConfig config;
  config.isServerSide = false;
  auto testHandler = std::make_unique<WebSocketHandler>(config);

  int msgCount = 0;
  WebSocketCallbacks newCallbacks;
  newCallbacks.onMessage = [&msgCount](std::span<const std::byte> /*payload*/, bool /*isBinary*/) { msgCount++; };
  testHandler->setCallbacks(std::move(newCallbacks));

  // Process a message
  auto frame = BuildUnmaskedFrame(Opcode::Text, "test");
  (void)testHandler->processInput(buf_bytes(frame), dummyState);

  EXPECT_EQ(msgCount, 1);
}

// ============================================================================
// Close initiated and then received tests
// ============================================================================

TEST_F(WebSocketHandlerTest, CloseInitiatedThenReceived) {
  // We send close first
  EXPECT_TRUE(handler->sendClose(CloseCode::Normal, "We close first"));
  EXPECT_TRUE(handler->isClosing());
  EXPECT_FALSE(handler->isCloseComplete());

  // Clear output
  auto output = handler->getPendingOutput();
  handler->onOutputWritten(output.size());

  // Peer responds with close
  RawBytes closeFrame;
  BuildCloseFrame(closeFrame, CloseCode::Normal, "Peer response");
  auto result = process(closeFrame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_TRUE(handler->isCloseComplete());
  EXPECT_EQ(closeCount, 1);
}

// ============================================================================
// RSV1 with compression tests
// ============================================================================

#ifdef AERONET_ENABLE_ZLIB

TEST_F(WebSocketHandlerTest, RSV1AcceptedWithCompression) {
  // Create handler with deflate compression enabled
  WebSocketConfig config;
  config.isServerSide = false;
  DeflateNegotiatedParams deflateParams{
      .serverMaxWindowBits = 15,
      .clientMaxWindowBits = 15,
      .serverNoContextTakeover = false,
      .clientNoContextTakeover = false,
  };

  int msgCount = 0;
  std::string receivedMessage;
  WebSocketCallbacks callbacks;
  callbacks.onMessage = [&](std::span<const std::byte> payload, bool /*isBinary*/) {
    msgCount++;
    receivedMessage = std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
  };

  auto compressHandler = std::make_unique<WebSocketHandler>(config, std::move(callbacks), deflateParams);

  // Create a compressed message
  DeflateContext ctx(deflateParams, DeflateConfig{}, false);
  std::string_view original = "Hello, compressed world!";
  RawBytes compressed;
  ASSERT_EQ(ctx.compress(sv_bytes(original), compressed), nullptr);

  // Build frame with RSV1 set (compressed)
  RawBytes frame;
  bool shouldMask = false;
  MaskingKey mask{};
  BuildFrame(frame, Opcode::Text, buf_bytes(compressed), true, shouldMask, mask, true);  // RSV1=true

  auto result = compressHandler->processInput(buf_bytes(frame), dummyState);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Continue);
  EXPECT_EQ(msgCount, 1);
  EXPECT_EQ(receivedMessage, original);
}

TEST_F(WebSocketHandlerTest, SendTextWithCompression) {
  // Create handler with deflate compression enabled
  WebSocketConfig config;
  config.isServerSide = true;  // Server side doesn't mask
  DeflateNegotiatedParams deflateParams{
      .serverMaxWindowBits = 15,
      .clientMaxWindowBits = 15,
      .serverNoContextTakeover = false,
      .clientNoContextTakeover = false,
  };

  auto compressHandler = std::make_unique<WebSocketHandler>(config, WebSocketCallbacks{}, deflateParams);

  // Send a compressible message
  std::string largeText(500, 'X');  // Repetitive data compresses well
  EXPECT_TRUE(compressHandler->sendText(largeText));
  EXPECT_TRUE(compressHandler->hasPendingOutput());

  auto output = compressHandler->getPendingOutput();
  // Compressed output should be smaller than original
  EXPECT_LT(output.size(), largeText.size() + 2);

  // First byte should have RSV1 set (0x41 = FIN + RSV1 + Text)
  EXPECT_EQ(static_cast<uint8_t>(output[0]) & 0x40, 0x40);
}

TEST_F(WebSocketHandlerTest, SendBinaryWithCompression) {
  WebSocketConfig config;
  config.isServerSide = true;
  DeflateNegotiatedParams deflateParams{
      .serverMaxWindowBits = 15,
      .clientMaxWindowBits = 15,
      .serverNoContextTakeover = false,
      .clientNoContextTakeover = false,
  };

  auto compressHandler = std::make_unique<WebSocketHandler>(config, WebSocketCallbacks{}, deflateParams);

  // Send compressible binary data
  std::array<std::byte, 500> binaryData;
  std::ranges::fill(binaryData, std::byte{0xAA});

  EXPECT_TRUE(compressHandler->sendBinary(binaryData));
  auto output = compressHandler->getPendingOutput();

  // Compressed should be smaller
  EXPECT_LT(output.size(), binaryData.size() + 2);
}

TEST_F(WebSocketHandlerTest, CompressionSkipsSmallPayloads) {
  WebSocketConfig config;
  config.isServerSide = true;
  config.deflateConfig.minCompressSize = 100;  // Don't compress small payloads
  DeflateNegotiatedParams deflateParams{
      .serverMaxWindowBits = 15,
      .clientMaxWindowBits = 15,
      .serverNoContextTakeover = false,
      .clientNoContextTakeover = false,
  };

  auto compressHandler = std::make_unique<WebSocketHandler>(config, WebSocketCallbacks{}, deflateParams);

  // Send small message
  EXPECT_TRUE(compressHandler->sendText("small"));
  auto output = compressHandler->getPendingOutput();

  // First byte should NOT have RSV1 set (not compressed)
  EXPECT_EQ(static_cast<uint8_t>(output[0]) & 0x40, 0x00);
}

TEST_F(WebSocketHandlerTest, DecompressionFailure) {
  WebSocketConfig config;
  config.isServerSide = false;
  DeflateNegotiatedParams deflateParams{
      .serverMaxWindowBits = 15,
      .clientMaxWindowBits = 15,
      .serverNoContextTakeover = false,
      .clientNoContextTakeover = false,
  };

  CloseCode errorCode = CloseCode::Normal;
  WebSocketCallbacks callbacks;
  callbacks.onError = [&errorCode](CloseCode code, std::string_view /*message*/) { errorCode = code; };

  auto compressHandler = std::make_unique<WebSocketHandler>(config, std::move(callbacks), deflateParams);

  // Build a frame with RSV1 (compressed) but with invalid compressed data
  RawBytes frame;
  frame.push_back(std::byte{0xC1});                              // FIN=1, RSV1=1, opcode=Text
  frame.push_back(std::byte{0x05});                              // MASK=0, length=5
  frame.append(reinterpret_cast<const std::byte*>("XXXXX"), 5);  // Invalid deflate data

  auto result = compressHandler->processInput(buf_bytes(frame), dummyState);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(errorCode, CloseCode::InvalidPayloadData);
}

TEST_F(WebSocketHandlerTest, CompressedFragmentedMessage) {
  WebSocketConfig config;
  config.isServerSide = false;
  DeflateNegotiatedParams deflateParams{
      .serverMaxWindowBits = 15,
      .clientMaxWindowBits = 15,
      .serverNoContextTakeover = false,
      .clientNoContextTakeover = false,
  };

  std::string receivedMessage;
  WebSocketCallbacks callbacks;
  callbacks.onMessage = [&receivedMessage](std::span<const std::byte> payload, bool /*isBinary*/) {
    receivedMessage = std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
  };

  auto compressHandler = std::make_unique<WebSocketHandler>(config, std::move(callbacks), deflateParams);

  // Compress full message
  DeflateContext ctx(deflateParams, DeflateConfig{}, false);
  std::string_view original = "Hello, fragmented compressed world!";
  RawBytes compressed;
  ASSERT_EQ(ctx.compress(sv_bytes(original), compressed), nullptr);

  // Split into two fragments - RSV1 should only be set on first
  std::size_t half = compressed.size() / 2;

  RawBytes frag1;
  frag1.push_back(std::byte{0x41});  // FIN=0, RSV1=1, opcode=Text
  frag1.push_back(std::byte{static_cast<uint8_t>(half)});
  frag1.append(compressed.data(), half);

  RawBytes frag2;
  frag2.push_back(std::byte{0x80});  // FIN=1, RSV1=0, opcode=Continuation
  frag2.push_back(std::byte{static_cast<uint8_t>(compressed.size() - half)});
  frag2.append(compressed.data() + half, compressed.size() - half);

  (void)compressHandler->processInput(buf_bytes(frag1), dummyState);
  auto result = compressHandler->processInput(buf_bytes(frag2), dummyState);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Continue);
  EXPECT_EQ(receivedMessage, original);
}

#endif  // AERONET_ENABLE_ZLIB

// ============================================================================
// Overlong UTF-8 encoding tests
// ============================================================================

TEST_F(WebSocketHandlerTest, Utf8OverlongEncoding2Byte) {
  // 2-byte encoding for a character that fits in 1 byte (0x00 = NUL)
  // Valid encoding: 0x00, overlong: 0xC0 0x80
  std::array<char, 2> overlong = {'\xC0', '\x80'};
  RawBytes frame;
  BuildFrame(frame, Opcode::Text, container_bytes(overlong), true, false);

  auto result = process(frame);
  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(lastErrorCode, CloseCode::InvalidPayloadData);
}

TEST_F(WebSocketHandlerTest, Utf8OverlongEncoding3Byte) {
  // 3-byte encoding for a character that fits in 2 bytes (0x80)
  // Valid encoding: 0xC2 0x80, overlong: 0xE0 0x82 0x80
  std::array<char, 3> overlong = {'\xE0', '\x82', '\x80'};
  RawBytes frame;
  BuildFrame(frame, Opcode::Text, container_bytes(overlong), true, false);

  auto result = process(frame);
  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_EQ(lastErrorCode, CloseCode::InvalidPayloadData);
}

TEST_F(WebSocketHandlerTest, Utf84ByteValid) {
  // Valid 4-byte sequence for U+1F600 (😀)
  std::array<char, 4> valid4byte = {'\xF0', '\x9F', '\x98', '\x80'};
  RawBytes frame;
  BuildFrame(frame, Opcode::Text, container_bytes(valid4byte), true, false);

  auto result = process(frame);
  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Continue);
  EXPECT_EQ(messageCount, 1);
}

// ============================================================================
// Close code in callback test
// ============================================================================

TEST_F(WebSocketHandlerTest, CloseCodeInCallback) {
  // Verify that sending a close sets up the state correctly
  handler->sendClose(CloseCode::GoingAway, "Shutting down");
  EXPECT_TRUE(handler->isClosing());
  EXPECT_FALSE(handler->isCloseComplete());
}

// ============================================================================
// Config accessor tests
// ============================================================================

TEST_F(WebSocketHandlerTest, ConfigAccessor) {
  WebSocketConfig config;
  config.isServerSide = true;
  config.maxMessageSize = 12345;
  auto testHandler = std::make_unique<WebSocketHandler>(config);

  EXPECT_TRUE(testHandler->config().isServerSide);
  EXPECT_EQ(testHandler->config().maxMessageSize, 12345);
}

// ============================================================================
// Close timeout tests
// ============================================================================

TEST_F(WebSocketHandlerTest, CloseTimeout_NotTimedOutInitially) {
  // Before close is initiated, should not be timed out
  EXPECT_FALSE(handler->hasCloseTimedOut());
}

TEST_F(WebSocketHandlerTest, CloseTimeout_NotTimedOutImmediatelyAfterClose) {
  handler->sendClose(CloseCode::Normal, "closing");
  // Immediately after, should not be timed out
  EXPECT_FALSE(handler->hasCloseTimedOut());
  EXPECT_TRUE(handler->isClosing());
}

TEST_F(WebSocketHandlerTest, CloseTimeout_TracksCloseInitiatedTime) {
  auto before = std::chrono::steady_clock::now();
  handler->sendClose(CloseCode::Normal, "closing");
  auto after = std::chrono::steady_clock::now();

  auto initiatedAt = handler->closeInitiatedAt();
  EXPECT_GE(initiatedAt, before);
  EXPECT_LE(initiatedAt, after);
}

TEST_F(WebSocketHandlerTest, CloseTimeout_WithVeryShortTimeout) {
  // Create a handler with a very short timeout
  WebSocketConfig config;
  config.isServerSide = false;
  config.closeTimeout = std::chrono::milliseconds{1};
  auto testHandler = std::make_unique<WebSocketHandler>(config);

  testHandler->sendClose(CloseCode::Normal, "closing");

  // Sleep a bit to ensure timeout
  std::this_thread::sleep_for(std::chrono::milliseconds{5});

  EXPECT_TRUE(testHandler->hasCloseTimedOut());
}

TEST_F(WebSocketHandlerTest, CloseTimeout_ForceCloseOnTimeout) {
  WebSocketConfig config;
  config.isServerSide = false;
  config.closeTimeout = std::chrono::milliseconds{1};
  auto testHandler = std::make_unique<WebSocketHandler>(config);

  testHandler->sendClose(CloseCode::Normal, "closing");
  EXPECT_TRUE(testHandler->isClosing());
  EXPECT_FALSE(testHandler->isCloseComplete());

  // Wait for timeout
  std::this_thread::sleep_for(std::chrono::milliseconds{5});

  EXPECT_TRUE(testHandler->hasCloseTimedOut());

  // Force close
  testHandler->forceCloseOnTimeout();
  EXPECT_TRUE(testHandler->isCloseComplete());
}

TEST_F(WebSocketHandlerTest, CloseTimeout_NotTimedOutAfterCloseComplete) {
  handler->sendClose(CloseCode::Normal, "closing");

  // Complete the close handshake by receiving close from peer
  RawBytes closeFrame;
  BuildCloseFrame(closeFrame, CloseCode::Normal, "peer closing");
  auto result = process(closeFrame);

  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Close);
  EXPECT_TRUE(handler->isCloseComplete());

  // Should not be timed out since handshake completed
  EXPECT_FALSE(handler->hasCloseTimedOut());
}

TEST_F(WebSocketHandlerTest, ForceCloseOnTimeout_NoOpIfNotClosing) {
  // forceCloseOnTimeout should do nothing if not in CloseSent state
  handler->forceCloseOnTimeout();
  EXPECT_FALSE(handler->isClosing());
  EXPECT_FALSE(handler->isCloseComplete());
}

}  // namespace aeronet::websocket
