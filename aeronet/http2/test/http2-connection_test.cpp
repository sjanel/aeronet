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

[[nodiscard]] vector<std::byte> SerializeFrame(FrameHeader header, std::span<const std::byte> payload) {
  vector<std::byte> out;
  out.resize(static_cast<decltype(out)::size_type>(FrameHeader::kSize + payload.size()));
  WriteFrameHeader(out.data(), header);
  std::ranges::copy(payload, out.begin() + static_cast<std::ptrdiff_t>(FrameHeader::kSize));
  return out;
}

void AdvanceToAwaitingSettingsAndDrainSettings(Http2Connection& conn) {
  auto preface = MakePreface();
  (void)conn.processInput(preface);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }
  ASSERT_EQ(conn.state(), ConnectionState::AwaitingSettings);
}

void AdvanceToOpenAndDrainSettingsAck(Http2Connection& conn) {
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  FrameHeader header;
  header.length = 0;
  header.type = FrameType::Settings;
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, std::span<const std::byte>{});
  auto res = conn.processInput(bytes);
  ASSERT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
  ASSERT_EQ(conn.state(), ConnectionState::Open);

  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }
}

constexpr auto kClosedStreamsMaxRetainedForTest = 16U;

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

// ============================
// Connection Preface Tests
// ============================

TEST(Http2Connection, ProcessValidPreface) {
  Http2Config config;
  Http2Connection conn(config, true);

  auto preface = MakePreface();
  auto result = conn.processInput(preface);

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
    EXPECT_EQ(header.streamId, 0U);
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

TEST(Http2Connection, SendRstStreamClosesAndDecrementsActiveStreamCount) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  uint32_t closedCount = 0;
  uint32_t resetCount = 0;
  conn.setOnStreamClosed([&](uint32_t) { ++closedCount; });
  conn.setOnStreamReset([&](uint32_t, ErrorCode) { ++resetCount; });

  const auto headerProvider = [](const HeaderCallback&) {};
  ASSERT_EQ(conn.sendHeaders(1, headerProvider, false), ErrorCode::NoError);
  EXPECT_EQ(conn.activeStreamCount(), 1U);

  conn.sendRstStream(1, ErrorCode::Cancel);

  EXPECT_EQ(conn.activeStreamCount(), 0U);
  EXPECT_EQ(closedCount, 1U);
  EXPECT_EQ(resetCount, 1U);
}

TEST(Http2Connection, RecvRstStreamClosesAndDecrementsActiveStreamCount) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  uint32_t closedCount = 0;
  uint32_t resetCount = 0;
  conn.setOnStreamClosed([&](uint32_t) { ++closedCount; });
  conn.setOnStreamReset([&](uint32_t, ErrorCode) { ++resetCount; });

  const auto headerProvider = [](const HeaderCallback&) {};
  ASSERT_EQ(conn.sendHeaders(1, headerProvider, false), ErrorCode::NoError);
  EXPECT_EQ(conn.activeStreamCount(), 1U);

  const uint32_t code = static_cast<uint32_t>(ErrorCode::Cancel);
  const std::array<std::byte, 4> payload = {
      std::byte{static_cast<uint8_t>((code >> 24) & 0xFF)}, std::byte{static_cast<uint8_t>((code >> 16) & 0xFF)},
      std::byte{static_cast<uint8_t>((code >> 8) & 0xFF)}, std::byte{static_cast<uint8_t>(code & 0xFF)}};
  FrameHeader header;
  header.length = payload.size();
  header.type = FrameType::RstStream;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_NE(res.action, Http2Connection::ProcessResult::Action::Error);

  EXPECT_EQ(conn.activeStreamCount(), 0U);
  EXPECT_EQ(closedCount, 1U);
  EXPECT_EQ(resetCount, 1U);
}

TEST(Http2Connection, DuplicateRstStreamDoesNotDoubleCloseAccounting) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  uint32_t closedCount = 0;
  uint32_t resetCount = 0;
  conn.setOnStreamClosed([&](uint32_t) { ++closedCount; });
  conn.setOnStreamReset([&](uint32_t, ErrorCode) { ++resetCount; });

  const auto headerProvider = [](const HeaderCallback&) {};
  ASSERT_EQ(conn.sendHeaders(1, headerProvider, false), ErrorCode::NoError);
  EXPECT_EQ(conn.activeStreamCount(), 1U);

  const uint32_t code = static_cast<uint32_t>(ErrorCode::Cancel);
  const std::array<std::byte, 4> payload = {
      std::byte{static_cast<uint8_t>((code >> 24) & 0xFF)}, std::byte{static_cast<uint8_t>((code >> 16) & 0xFF)},
      std::byte{static_cast<uint8_t>((code >> 8) & 0xFF)}, std::byte{static_cast<uint8_t>(code & 0xFF)}};
  FrameHeader header;
  header.length = payload.size();
  header.type = FrameType::RstStream;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  // First RST_STREAM closes the stream.
  {
    auto bytes = SerializeFrame(header, payload);
    auto res = conn.processInput(bytes);
    EXPECT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
  }

  EXPECT_EQ(conn.activeStreamCount(), 0U);
  EXPECT_EQ(closedCount, 1U);
  EXPECT_EQ(resetCount, 1U);

  // Duplicate RST_STREAM on an already closed (but retained) stream must not re-close.
  {
    auto bytes = SerializeFrame(header, payload);
    auto res = conn.processInput(bytes);
    EXPECT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
  }

  EXPECT_EQ(conn.activeStreamCount(), 0U);
  EXPECT_EQ(closedCount, 1U);
  EXPECT_EQ(resetCount, 2U);
}

TEST(Http2Connection, ClosedStreamsArePrunedFromMapAfterRetentionLimit) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  const auto headerProvider = [](const HeaderCallback&) {};

  // Close more streams than the retention FIFO keeps.
  const uint32_t streamCountToClose = kClosedStreamsMaxRetainedForTest + 2U;
  for (uint32_t idx = 0; idx < streamCountToClose; ++idx) {
    const uint32_t streamId = 1U + (idx * 2U);
    ASSERT_EQ(conn.sendHeaders(streamId, headerProvider, false), ErrorCode::NoError);
    EXPECT_EQ(conn.activeStreamCount(), 1U);
    conn.sendRstStream(streamId, ErrorCode::Cancel);
    EXPECT_EQ(conn.activeStreamCount(), 0U);
  }

  // Oldest stream should have been pruned.
  const std::array<std::byte, 1> dummy = {std::byte{0}};
  EXPECT_EQ(conn.sendData(1U, dummy, false), ErrorCode::ProtocolError);

  // Most recent stream should still be retained (but closed).
  const uint32_t lastStreamId = 1U + ((streamCountToClose - 1U) * 2U);
  EXPECT_EQ(conn.sendData(lastStreamId, dummy, false), ErrorCode::StreamClosed);
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

// ============================
// Frame processing error paths (connection errors)
// ============================

TEST(Http2Connection, SettingsFrameOnNonZeroStreamIsProtocolError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  FrameHeader header;
  header.length = 0;
  header.type = FrameType::Settings;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, std::span<const std::byte>{});
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
  EXPECT_TRUE(conn.hasPendingOutput());
  const auto out = conn.getPendingOutput();
  ASSERT_GE(out.size(), FrameHeader::kSize);
  EXPECT_EQ(ParseFrameHeader(out).type, FrameType::GoAway);
}

TEST(Http2Connection, SettingsFrameInvalidLengthIsFrameSizeError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  std::array<std::byte, 5> payload = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
  FrameHeader header;
  header.length = payload.size();
  header.type = FrameType::Settings;
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FrameSizeError);
}

TEST(Http2Connection, SettingsFrameInvalidEnablePushIsProtocolError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  // ENABLE_PUSH must be 0 or 1.
  std::array<std::byte, 6> entry = {
      std::byte{0x00}, std::byte{0x02},                                   // SETTINGS_ENABLE_PUSH
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x02}  // value=2
  };

  FrameHeader header;
  header.length = entry.size();
  header.type = FrameType::Settings;
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, entry);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

TEST(Http2Connection, SettingsFrameInvalidMaxFrameSizeIsProtocolError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  // MAX_FRAME_SIZE must be in [16384, 16777215]. Provide 16383.
  std::array<std::byte, 6> entry = {
      std::byte{0x00}, std::byte{0x05},                                   // SETTINGS_MAX_FRAME_SIZE
      std::byte{0x00}, std::byte{0x00}, std::byte{0x3F}, std::byte{0xFF}  // 16383
  };

  FrameHeader header;
  header.length = entry.size();
  header.type = FrameType::Settings;
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, entry);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

TEST(Http2Connection, PingFrameOnNonZeroStreamIsProtocolError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  std::array<std::byte, 8> payload = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                                      std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
  FrameHeader header;
  header.length = payload.size();
  header.type = FrameType::Ping;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

TEST(Http2Connection, PingFrameInvalidLengthIsFrameSizeError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  std::array<std::byte, 7> payload = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                                      std::byte{5}, std::byte{6}, std::byte{7}};
  FrameHeader header;
  header.length = payload.size();
  header.type = FrameType::Ping;
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FrameSizeError);
}

TEST(Http2Connection, GoAwayFrameInvalidLengthIsFrameSizeError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  std::array<std::byte, 7> payload = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                                      std::byte{0}, std::byte{0}, std::byte{0}};
  FrameHeader header;
  header.length = payload.size();
  header.type = FrameType::GoAway;
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FrameSizeError);
}

TEST(Http2Connection, WindowUpdateInvalidLengthIsFrameSizeError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  std::array<std::byte, 3> payload = {std::byte{0}, std::byte{0}, std::byte{1}};
  FrameHeader header;
  header.length = payload.size();
  header.type = FrameType::WindowUpdate;
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FrameSizeError);
}

// ============================
// Frame processing error paths (stream errors)
// ============================

TEST(Http2Connection, WindowUpdateZeroIncrementOnStreamSendsRstStream) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  std::array<std::byte, 4> payload = {std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
  FrameHeader header;
  header.length = payload.size();
  header.type = FrameType::WindowUpdate;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::OutputReady);
  EXPECT_EQ(res.errorCode, ErrorCode::NoError);
  ASSERT_TRUE(conn.hasPendingOutput());
  const auto out = conn.getPendingOutput();
  ASSERT_GE(out.size(), FrameHeader::kSize);
  const auto outHeader = ParseFrameHeader(out);
  EXPECT_EQ(outHeader.type, FrameType::RstStream);
  ASSERT_EQ(outHeader.length, 4U);

  RstStreamFrame rst;
  const auto payloadView = out.subspan(FrameHeader::kSize, outHeader.length);
  ASSERT_EQ(ParseRstStreamFrame(outHeader, payloadView, rst), FrameParseResult::Ok);
  EXPECT_EQ(rst.errorCode, ErrorCode::ProtocolError);
}

TEST(Http2Connection, UnexpectedContinuationFrameIsProtocolError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  std::array<std::byte, 1> payload = {std::byte{0x00}};
  FrameHeader header;
  header.length = payload.size();
  header.type = FrameType::Continuation;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

TEST(Http2Connection, UnexpectedPushPromiseIsProtocolError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  std::array<std::byte, 4> payload = {std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};
  FrameHeader header;
  header.length = payload.size();
  header.type = FrameType::PushPromise;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

}  // namespace
}  // namespace aeronet::http2