#include "aeronet/http2-connection.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

#include "aeronet/http2-config.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::http2 {

namespace {

// Helper to create connection preface as bytes
vector<std::byte> MakePreface() {
  vector<std::byte> preface;
  preface.reserve(kConnectionPreface.size());
  std::ranges::transform(kConnectionPreface, std::back_inserter(preface),
                         [](char ch) { return static_cast<std::byte>(ch); });
  return preface;
}

// ============================
// Connection State Tests
// ============================

TEST(Http2Connection, InitialState) {
  Http2Config config;
  Http2Connection conn(config, true);

  EXPECT_EQ(conn.state(), ConnectionState::AwaitingPreface);
  EXPECT_FALSE(conn.isOpen());
  EXPECT_FALSE(conn.canCreateStreams());
  EXPECT_EQ(conn.activeStreamCount(), 0);
  EXPECT_EQ(conn.lastPeerStreamId(), 0);
  EXPECT_EQ(conn.lastLocalStreamId(), 0);
}

TEST(Http2Connection, ConnectionStateName) {
  EXPECT_EQ(ConnectionStateName(ConnectionState::AwaitingPreface), "awaiting_preface");
  EXPECT_EQ(ConnectionStateName(ConnectionState::AwaitingSettings), "awaiting_settings");
  EXPECT_EQ(ConnectionStateName(ConnectionState::Open), "open");
  EXPECT_EQ(ConnectionStateName(ConnectionState::GoAwaySent), "goaway_sent");
  EXPECT_EQ(ConnectionStateName(ConnectionState::GoAwayReceived), "goaway_received");
  EXPECT_EQ(ConnectionStateName(ConnectionState::Closed), "closed");
  EXPECT_EQ(ConnectionStateName(static_cast<ConnectionState>(static_cast<std::underlying_type_t<ConnectionState>>(-1))),
            "unknown");
}

// ============================
// Connection Preface Tests
// ============================

TEST(Http2Connection, ProcessValidPreface) {
  Http2Config config;
  Http2Connection conn(config, true);

  auto preface = MakePreface();
  auto result = conn.processInput(std::span<const std::byte>(preface.data(), preface.size()));

  EXPECT_EQ(result.action, Http2Connection::ProcessResult::Action::OutputReady);
  EXPECT_EQ(result.bytesConsumed, preface.size());
  EXPECT_EQ(conn.state(), ConnectionState::AwaitingSettings);
}

TEST(Http2Connection, ProcessPartialPreface) {
  Http2Config config;
  Http2Connection conn(config, true);

  auto preface = MakePreface();
  // Only send half of the preface
  auto result = conn.processInput(std::span<const std::byte>(preface.data(), preface.size() / 2));

  EXPECT_EQ(result.action, Http2Connection::ProcessResult::Action::Continue);
  EXPECT_EQ(result.bytesConsumed, 0);
  EXPECT_EQ(conn.state(), ConnectionState::AwaitingPreface);
}

TEST(Http2Connection, ProcessInvalidPreface) {
  Http2Config config;
  Http2Connection conn(config, true);

  std::array<std::byte, 24> invalidPreface{};  // All zeros
  auto result = conn.processInput(invalidPreface);

  EXPECT_EQ(result.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(result.errorCode, ErrorCode::ProtocolError);
}

// ============================
// Settings Exchange Tests
// ============================

TEST(Http2Connection, ServerSendsSettingsAfterPreface) {
  Http2Config config;
  config.maxConcurrentStreams = 50;
  config.initialWindowSize = 32768;

  Http2Connection conn(config, true);

  auto preface = MakePreface();
  (void)conn.processInput(std::span<const std::byte>(preface.data(), preface.size()));

  // Server should have sent SETTINGS
  auto pendingOutput = conn.getPendingOutput();
  EXPECT_FALSE(pendingOutput.empty());

  // First frame should be SETTINGS
  if (pendingOutput.size() >= FrameHeader::kSize) {
    FrameHeader header = ParseFrameHeader(pendingOutput);
    EXPECT_EQ(header.type, FrameType::Settings);
    EXPECT_EQ(header.streamId, 0u);
  }
}

// ============================
// Output Buffer Tests
// ============================

TEST(Http2Connection, OnOutputWritten) {
  Http2Config config;
  Http2Connection conn(config, true);

  auto preface = MakePreface();
  (void)conn.processInput(std::span<const std::byte>(preface.data(), preface.size()));

  EXPECT_TRUE(conn.hasPendingOutput());

  auto output = conn.getPendingOutput();
  std::size_t outputSize = output.size();

  conn.onOutputWritten(outputSize);

  EXPECT_FALSE(conn.hasPendingOutput());
}

// ============================
// Stream Management Tests
// ============================

TEST(Http2Connection, GetStreamNotFound) {
  Http2Config config;
  Http2Connection conn(config, true);

  EXPECT_EQ(conn.getStream(1), nullptr);
}

// ============================
// GOAWAY Tests
// ============================

TEST(Http2Connection, InitiateGoAway) {
  Http2Config config;
  Http2Connection conn(config, true);

  // First establish connection
  auto preface = MakePreface();
  (void)conn.processInput(std::span<const std::byte>(preface.data(), preface.size()));
  conn.onOutputWritten(conn.getPendingOutput().size());

  // Initiate GOAWAY
  conn.initiateGoAway(ErrorCode::NoError, "graceful shutdown");

  EXPECT_EQ(conn.state(), ConnectionState::GoAwaySent);
  EXPECT_TRUE(conn.hasPendingOutput());

  // Check that GOAWAY frame was queued
  auto output = conn.getPendingOutput();
  if (output.size() >= FrameHeader::kSize) {
    FrameHeader header = ParseFrameHeader(output);
    EXPECT_EQ(header.type, FrameType::GoAway);
    EXPECT_EQ(header.streamId, 0);
  }
}

TEST(Http2Connection, DoubleGoAwayIgnored) {
  Http2Config config;
  Http2Connection conn(config, true);

  auto preface = MakePreface();
  (void)conn.processInput(std::span<const std::byte>(preface.data(), preface.size()));
  conn.onOutputWritten(conn.getPendingOutput().size());

  conn.initiateGoAway(ErrorCode::NoError);
  EXPECT_EQ(conn.state(), ConnectionState::GoAwaySent);

  std::size_t outputSize = conn.getPendingOutput().size();

  // Second GOAWAY should be ignored
  conn.initiateGoAway(ErrorCode::InternalError);

  // Output size shouldn't increase
  EXPECT_EQ(conn.getPendingOutput().size(), outputSize);
}

// ============================
// Settings Tests
// ============================

TEST(Http2Connection, LocalSettings) {
  Http2Config config;
  config.maxConcurrentStreams = 200;
  config.initialWindowSize = 1048576;
  config.maxFrameSize = 32768;

  Http2Connection conn(config, true);

  const auto& localSettings = conn.localSettings();
  EXPECT_EQ(localSettings.maxConcurrentStreams, 200U);
  EXPECT_EQ(localSettings.initialWindowSize, 1048576U);
  EXPECT_EQ(localSettings.maxFrameSize, 32768U);
}

TEST(Http2Connection, DefaultPeerSettings) {
  Http2Config config;
  Http2Connection conn(config, true);

  const auto& peerSettings = conn.peerSettings();
  EXPECT_EQ(peerSettings.headerTableSize, 4096U);
  EXPECT_TRUE(peerSettings.enablePush);
  EXPECT_EQ(peerSettings.maxConcurrentStreams, 100U);
  EXPECT_EQ(peerSettings.initialWindowSize, 65535U);
  EXPECT_EQ(peerSettings.maxFrameSize, 16384U);
}

// ============================
// Flow Control Tests
// ============================

TEST(Http2Connection, ConnectionFlowControl) {
  Http2Config config;
  config.connectionWindowSize = 1048576;  // 1MB

  Http2Connection conn(config, true);

  // Initial send window is the RFC default (65535)
  EXPECT_EQ(conn.connectionSendWindow(), 65535);

  // Receive window should be set to our configured value
  EXPECT_EQ(conn.connectionRecvWindow(), 1048576);
}

// ============================
// Callback Tests
// ============================

TEST(Http2Connection, SetCallbacks) {
  Http2Config config;
  Http2Connection conn(config, true);

  bool headersCalled = false;
  bool dataCalled = false;
  bool resetCalled = false;
  bool closedCalled = false;
  bool goawayCalled = false;

  conn.setOnHeaders([&](uint32_t, const HeaderProvider&, bool) { headersCalled = true; });
  conn.setOnData([&](uint32_t, std::span<const std::byte>, bool) { dataCalled = true; });
  conn.setOnStreamReset([&](uint32_t, ErrorCode) { resetCalled = true; });
  conn.setOnStreamClosed([&](uint32_t) { closedCalled = true; });
  conn.setOnGoAway([&](uint32_t, ErrorCode, std::string_view) { goawayCalled = true; });

  // Callbacks are set but not called yet
  EXPECT_FALSE(headersCalled);
  EXPECT_FALSE(dataCalled);
  EXPECT_FALSE(resetCalled);
  EXPECT_FALSE(closedCalled);
  EXPECT_FALSE(goawayCalled);
}

// ============================
// PING Tests
// ============================

TEST(Http2Connection, SendPing) {
  Http2Config config;
  Http2Connection conn(config, true);

  // First establish connection
  auto preface = MakePreface();
  (void)conn.processInput(std::span<const std::byte>(preface.data(), preface.size()));
  conn.onOutputWritten(conn.getPendingOutput().size());

  std::array<std::byte, 8> opaqueData = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                                         std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
  conn.sendPing(opaqueData, false);

  EXPECT_TRUE(conn.hasPendingOutput());

  auto output = conn.getPendingOutput();
  if (output.size() >= FrameHeader::kSize) {
    FrameHeader header = ParseFrameHeader(output);
    EXPECT_EQ(header.type, FrameType::Ping);
    EXPECT_EQ(header.length, 8U);
    EXPECT_EQ(header.streamId, 0);
  }
}

// ============================
// Window Update Tests
// ============================

TEST(Http2Connection, SendWindowUpdate) {
  Http2Config config;
  Http2Connection conn(config, true);

  auto preface = MakePreface();
  (void)conn.processInput(std::span<const std::byte>(preface.data(), preface.size()));
  conn.onOutputWritten(conn.getPendingOutput().size());

  int32_t initialWindow = conn.connectionRecvWindow();
  conn.sendWindowUpdate(0, 4096);

  EXPECT_EQ(conn.connectionRecvWindow(), initialWindow + 4096);
  EXPECT_TRUE(conn.hasPendingOutput());
}

// ============================
// RST_STREAM Tests
// ============================

TEST(Http2Connection, SendRstStream) {
  Http2Config config;
  Http2Connection conn(config, true);

  auto preface = MakePreface();
  (void)conn.processInput(std::span<const std::byte>(preface.data(), preface.size()));
  conn.onOutputWritten(conn.getPendingOutput().size());

  conn.sendRstStream(1, ErrorCode::Cancel);

  EXPECT_TRUE(conn.hasPendingOutput());

  auto output = conn.getPendingOutput();
  if (output.size() >= FrameHeader::kSize) {
    FrameHeader header = ParseFrameHeader(output);
    EXPECT_EQ(header.type, FrameType::RstStream);
    EXPECT_EQ(header.length, 4U);
    EXPECT_EQ(header.streamId, 1U);
  }
}

// ============================
// Empty Input Tests
// ============================

TEST(Http2Connection, ProcessEmptyInput) {
  Http2Config config;
  Http2Connection conn(config, true);

  auto result = conn.processInput(std::span<const std::byte>{});

  EXPECT_EQ(result.action, Http2Connection::ProcessResult::Action::Continue);
  EXPECT_EQ(result.bytesConsumed, 0);
}

}  // namespace
}  // namespace aeronet::http2