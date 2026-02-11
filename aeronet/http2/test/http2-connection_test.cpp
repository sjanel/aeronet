#include "aeronet/http2-connection.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "aeronet/headers-view-map.hpp"
#include "aeronet/hpack.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/http2-stream.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::http2 {

namespace {

struct WireDecodedHeadersDebug {
  bool foundHeaders{false};
  bool decodeSuccess{false};
  bool hasDate{false};
  std::vector<std::byte> headerBlockBytes;
  std::vector<std::pair<std::string, std::string>> headers;
};

[[nodiscard]] WireDecodedHeadersDebug DecodeFirstHeadersFromOutput(std::span<const std::byte> output) {
  HpackDecoder decoder(4096);

  RawBytes headerBlock;
  std::size_t pos = 0;

  while (output.size() - pos >= FrameHeader::kSize) {
    const auto header = ParseFrameHeader(output.subspan(pos));
    const std::size_t totalFrameSize = FrameHeader::kSize + header.length;
    if (output.size() - pos < totalFrameSize) {
      break;
    }

    const auto payload = output.subspan(pos + FrameHeader::kSize, header.length);

    if (header.type == FrameType::Headers) {
      WireDecodedHeadersDebug dbg;
      dbg.foundHeaders = true;

      HeadersFrame headersFrame;
      if (ParseHeadersFrame(header, payload, headersFrame) != FrameParseResult::Ok) {
        return dbg;
      }

      headerBlock.assign(headersFrame.headerBlockFragment);

      // Gather CONTINUATION frames if needed.
      std::size_t nextPos = pos + totalFrameSize;
      while (!headersFrame.endHeaders) {
        if (output.size() - nextPos < FrameHeader::kSize) {
          return dbg;
        }
        const auto contHeader = ParseFrameHeader(output.subspan(nextPos));
        const std::size_t contTotalSize = FrameHeader::kSize + contHeader.length;
        if (output.size() - nextPos < contTotalSize) {
          return dbg;
        }
        if (contHeader.type != FrameType::Continuation || contHeader.streamId != header.streamId) {
          return dbg;
        }

        ContinuationFrame continuation;
        ParseContinuationFrame(contHeader, output.subspan(nextPos + FrameHeader::kSize, contHeader.length),
                               continuation);
        headerBlock.append(std::span<const std::byte>(continuation.headerBlockFragment));
        headersFrame.endHeaders = continuation.endHeaders;

        nextPos += contTotalSize;
      }

      dbg.headerBlockBytes.assign(headerBlock.begin(), headerBlock.end());

      auto decodeResult = decoder.decode(std::span<const std::byte>(headerBlock));
      dbg.decodeSuccess = decodeResult.isSuccess();
      if (!dbg.decodeSuccess) {
        return dbg;
      }

      dbg.hasDate = decodeResult.decodedHeaders.contains(http::Date);
      dbg.headers.reserve(decodeResult.decodedHeaders.size());
      for (const auto& [name, value] : decodeResult.decodedHeaders) {
        dbg.headers.emplace_back(std::string{name}, std::string{value});
      }
      return dbg;
    }

    pos += totalFrameSize;
  }

  return {};
}

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

TEST(Http2Connection, ResponseHeadersIncludeDateWhenBodyFollows) {
  Http2Config config;
  Http2Connection server(config, true);
  Http2Connection client(config, false);

  // Complete minimal HTTP/2 preface + SETTINGS exchange.
  client.sendClientPreface();
  ASSERT_TRUE(client.hasPendingOutput());
  {
    const auto out = client.getPendingOutput();
    auto res = server.processInput(out);
    ASSERT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
    client.onOutputWritten(out.size());
  }

  // Server should respond with its SETTINGS.
  ASSERT_TRUE(server.hasPendingOutput());
  {
    const auto out = server.getPendingOutput();
    auto res = client.processInput(out);
    ASSERT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
    server.onOutputWritten(out.size());
  }

  // Drain any ACK/housekeeping output in both directions.
  for (int iter = 0; iter < 64 && (client.hasPendingOutput() || server.hasPendingOutput()); ++iter) {
    if (client.hasPendingOutput()) {
      const auto out = client.getPendingOutput();
      auto res = server.processInput(out);
      ASSERT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
      client.onOutputWritten(out.size());
    }
    if (server.hasPendingOutput()) {
      const auto out = server.getPendingOutput();
      auto res = client.processInput(out);
      ASSERT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
      server.onOutputWritten(out.size());
    }
  }

  ASSERT_TRUE(server.isOpen());
  ASSERT_TRUE(client.isOpen());

  ASSERT_FALSE(server.hasPendingOutput());
  ASSERT_FALSE(client.hasPendingOutput());

  // Capture decoded response headers.
  HeadersViewMap decoded;
  bool gotHeaders = false;
  client.setOnHeadersDecoded([&](uint32_t /*streamId*/, const HeadersViewMap& headers, bool /*endStream*/) {
    decoded = headers;
    gotHeaders = true;
  });

  // Send response HEADERS (no END_STREAM) + DATA (END_STREAM).
  const uint32_t streamId = 1;
  const std::array<char, RFC7231DateStrLen> dateBuf = [] {
    std::array<char, RFC7231DateStrLen> buf{};
    (void)TimeToStringRFC7231(SysClock::now(), buf.data());
    return buf;
  }();

  RawChars headers;
  headers.append(MakeHttp1HeaderLine(":status", "200"));
  headers.append(MakeHttp1HeaderLine("content-type", "text/plain"));
  headers.append(MakeHttp1HeaderLine("x-custom", "original"));
  headers.append(MakeHttp1HeaderLine("x-another", "anothervalue"));
  headers.append(MakeHttp1HeaderLine("x-global", "gvalue"));
  headers.append(MakeHttp1HeaderLine("date", std::string_view{dateBuf.data(), RFC7231DateStrLen}));
  headers.append(MakeHttp1HeaderLine("content-length", "1"));

  ASSERT_EQ(server.sendHeaders(streamId, http::StatusCode{}, HeadersView(headers), false), ErrorCode::NoError);

  // Sanity-check: decode the outgoing HEADERS directly from the server output *before* sending DATA.
  ASSERT_TRUE(server.hasPendingOutput());
  {
    const auto out = server.getPendingOutput();
    const auto wire = DecodeFirstHeadersFromOutput(out);
    ASSERT_TRUE(wire.foundHeaders) << "No HEADERS frame found in server output";
    ASSERT_TRUE(wire.decodeSuccess) << "Failed to HPACK-decode server HEADERS";

    // Compare the raw HPACK header block against a locally generated expected block.
    HpackEncoder expectedEncoder(4096);
    RawBytes expectedBlock;
    expectedEncoder.encode(expectedBlock, ":status", "200");
    expectedEncoder.encode(expectedBlock, "content-type", "text/plain");
    expectedEncoder.encode(expectedBlock, "x-custom", "original");
    expectedEncoder.encode(expectedBlock, "x-another", "anothervalue");
    expectedEncoder.encode(expectedBlock, "x-global", "gvalue");
    expectedEncoder.encode(expectedBlock, "date", std::string_view{dateBuf.data(), RFC7231DateStrLen});
    expectedEncoder.encode(expectedBlock, "content-length", "1");

    ASSERT_EQ(wire.headerBlockBytes.size(), expectedBlock.size()) << "Server HPACK block size differs from expected";
    ASSERT_TRUE(std::equal(wire.headerBlockBytes.begin(), wire.headerBlockBytes.end(), expectedBlock.begin()))
        << "Server HPACK block bytes differ from expected";

    if (!wire.hasDate) {
      for (const auto& [name, value] : wire.headers) {
        ADD_FAILURE() << "Wire-decoded header: '" << name << "'='" << value << "'";
      }
      FAIL() << "Missing 'date' in wire-decoded headers";
    }
  }

  const std::array<std::byte, 1> body = {std::byte{'R'}};
  ASSERT_EQ(server.sendData(streamId, body, true), ErrorCode::NoError);

  // Deliver all server output to client.
  while (server.hasPendingOutput()) {
    const auto out = server.getPendingOutput();
    auto res = client.processInput(out);
    ASSERT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
    server.onOutputWritten(out.size());
  }

  ASSERT_TRUE(gotHeaders);
  auto it = decoded.find(http::Date);
  if (it == decoded.end()) {
    for (const auto& [name, value] : decoded) {
      ADD_FAILURE() << "Decoded header: '" << name << "'='" << value << "'";
    }
    FAIL() << "Missing 'date' in decoded headers";
  }
  EXPECT_EQ(it->second.size(), RFC7231DateStrLen);
  EXPECT_TRUE(it->second.ends_with("GMT"));
  EXPECT_NE(TryParseTimeRFC7231(it->second), kInvalidTimePoint);
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

  ASSERT_EQ(conn.sendHeaders(1, http::StatusCode{}, HeadersView(std::string{}), false), ErrorCode::NoError);
  // Drain any output produced by sendHeaders so we can observe the RST_STREAM from WINDOW_UPDATE
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }
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

  ASSERT_EQ(conn.sendHeaders(1, http::StatusCodeOK, HeadersView(std::string{}), false), ErrorCode::NoError);
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

  ASSERT_EQ(conn.sendHeaders(1, http::StatusCodeOK, HeadersView(std::string{}), false), ErrorCode::NoError);
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

  // Close more streams than the retention FIFO keeps.
  const uint32_t streamCountToClose = kClosedStreamsMaxRetainedForTest + 2U;
  for (uint32_t idx = 0; idx < streamCountToClose; ++idx) {
    const uint32_t streamId = 1U + (idx * 2U);
    ASSERT_EQ(conn.sendHeaders(streamId, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);
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

  conn.setOnHeadersDecoded([&](uint32_t, const HeadersViewMap&, bool) { headersCalled = true; });
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
  // Search pending output for a GOAWAY frame
  bool foundGoAway = false;
  std::size_t pos = 0;
  while (pos + FrameHeader::kSize <= out.size()) {
    FrameHeader fh = ParseFrameHeader(out.subspan(pos, FrameHeader::kSize));
    if (fh.type == FrameType::GoAway) {
      foundGoAway = true;
      break;
    }
    pos += FrameHeader::kSize + fh.length;
  }
  EXPECT_TRUE(foundGoAway);
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

TEST(Http2Connection, SettingsInitialWindowSizeTooLargeIsFlowControlError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  // SETTINGS_INITIAL_WINDOW_SIZE (0x04) with value 0x80000000 (> 0x7FFFFFFF)
  std::array<std::byte, 6> entry = {std::byte{0x00}, std::byte{0x04},  // SETTINGS_INITIAL_WINDOW_SIZE
                                    std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};

  FrameHeader header;
  header.length = entry.size();
  header.type = FrameType::Settings;
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, entry);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FlowControlError);
  EXPECT_TRUE(conn.hasPendingOutput());
  const auto out = conn.getPendingOutput();
  ASSERT_GE(out.size(), FrameHeader::kSize);
  bool foundGoAway2 = false;
  std::size_t pos2 = 0;
  while (pos2 + FrameHeader::kSize <= out.size()) {
    FrameHeader fh = ParseFrameHeader(out.subspan(pos2, FrameHeader::kSize));
    if (fh.type == FrameType::GoAway) {
      foundGoAway2 = true;
      break;
    }
    pos2 += FrameHeader::kSize + fh.length;
  }
  EXPECT_TRUE(foundGoAway2);
}

TEST(Http2Connection, UnknownSettingsParameterIsIgnored) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  // Use an unknown SETTINGS parameter ID (0xFFFF) with an arbitrary value=1
  std::array<std::byte, 6> entry = {std::byte{0xFF}, std::byte{0xFF},  // unknown ID 0xFFFF
                                    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};

  FrameHeader header;
  header.length = entry.size();
  header.type = FrameType::Settings;
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, entry);
  auto res = conn.processInput(bytes);

  // Should not be a connection error and connection should transition to Open
  EXPECT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(conn.state(), ConnectionState::Open);

  // A SETTINGS ACK should be emitted
  EXPECT_TRUE(conn.hasPendingOutput());
  const auto out = conn.getPendingOutput();
  ASSERT_GE(out.size(), FrameHeader::kSize);

  bool foundSettingsAck = false;
  std::size_t pos = 0;
  while (pos + FrameHeader::kSize <= out.size()) {
    FrameHeader fh = ParseFrameHeader(out.subspan(pos, FrameHeader::kSize));
    if (fh.type == FrameType::Settings && fh.flags == FrameFlags::SettingsAck) {
      foundSettingsAck = true;
      break;
    }
    pos += FrameHeader::kSize + fh.length;
  }
  EXPECT_TRUE(foundSettingsAck);
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

TEST(Http2Connection, PingAckFrameIsAcceptedAndNoResponseSent) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  std::array<std::byte, 8> payload = {std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD},
                                      std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
  FrameHeader header;
  header.length = payload.size();
  header.type = FrameType::Ping;
  header.flags = FrameFlags::PingAck;  // ACK set
  header.streamId = 0;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);

  // Handler should accept the PING ACK and not produce output
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Continue);
  EXPECT_EQ(res.errorCode, ErrorCode::NoError);
  EXPECT_FALSE(conn.hasPendingOutput());
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

TEST(Http2Connection, GoAwayFrameOnNonZeroStreamIsProtocolError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  // Minimal valid GOAWAY payload is 8 bytes (last-stream-id + error-code) optionally with debug data
  std::array<std::byte, 8> payload = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                                      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
  FrameHeader header;
  header.length = payload.size();
  header.type = FrameType::GoAway;
  header.flags = FrameFlags::None;
  header.streamId = 1;  // Non-zero stream id should be protocol error

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
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

TEST(Http2Connection, WindowUpdateConnectionOverflowIsFlowControlError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  // WINDOW_UPDATE payload is 4 bytes. Use increment 0x7FFFFFFF to cause newWindow > 0x7FFFFFFF
  const uint32_t increment = 0x7FFFFFFFU;
  std::array<std::byte, 4> payload = {std::byte{static_cast<uint8_t>((increment >> 24) & 0xFF)},
                                      std::byte{static_cast<uint8_t>((increment >> 16) & 0xFF)},
                                      std::byte{static_cast<uint8_t>((increment >> 8) & 0xFF)},
                                      std::byte{static_cast<uint8_t>(increment & 0xFF)}};

  FrameHeader header;
  header.length = payload.size();
  header.type = FrameType::WindowUpdate;
  header.flags = FrameFlags::None;
  header.streamId = 0;  // connection-level

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);

  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FlowControlError);

  // A GOAWAY should be queued as part of connectionError handling
  EXPECT_TRUE(conn.hasPendingOutput());
  const auto out = conn.getPendingOutput();
  ASSERT_GE(out.size(), FrameHeader::kSize);
  bool foundGoAway = false;
  std::size_t pos = 0;
  while (pos + FrameHeader::kSize <= out.size()) {
    FrameHeader fh = ParseFrameHeader(out.subspan(pos, FrameHeader::kSize));
    if (fh.type == FrameType::GoAway) {
      foundGoAway = true;
      break;
    }
    pos += FrameHeader::kSize + fh.length;
  }
  EXPECT_TRUE(foundGoAway);
}

TEST(Http2Connection, SettingsInitialWindowSizeTooSmallCausesStreamOverflow) {
  Http2Config config;
  config.connectionWindowSize = 1048576;  // large connection window to allow sending

  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Create a stream by sending headers
  ASSERT_EQ(conn.sendHeaders(1, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);

  // Consume the stream send window by sending exactly the peer initial window bytes
  const uint32_t initialWindow = conn.peerSettings().initialWindowSize;
  std::vector<std::byte> payload(initialWindow, std::byte{0});

  EXPECT_EQ(conn.sendData(1U, payload, false), ErrorCode::NoError);

  // Now send SETTINGS_INITIAL_WINDOW_SIZE = 0 which should cause updateInitialWindowSize
  // to compute newWindow < 0 for the existing stream and return FlowControlError.
  std::array<std::byte, 6> entry = {std::byte{0x00}, std::byte{0x04},  // SETTINGS_INITIAL_WINDOW_SIZE
                                    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};

  FrameHeader header;
  header.length = entry.size();
  header.type = FrameType::Settings;
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, entry);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FlowControlError);
  EXPECT_TRUE(conn.hasPendingOutput());
  const auto out = conn.getPendingOutput();
  ASSERT_GE(out.size(), FrameHeader::kSize);
  bool foundGoAway = false;
  std::size_t pos = 0;
  while (pos + FrameHeader::kSize <= out.size()) {
    FrameHeader fh = ParseFrameHeader(out.subspan(pos, FrameHeader::kSize));
    if (fh.type == FrameType::GoAway) {
      foundGoAway = true;
      break;
    }
    pos += FrameHeader::kSize + fh.length;
  }
  EXPECT_TRUE(foundGoAway);
}

TEST(Http2Connection, SettingsInitialWindowSizeStreamWindowOverflow) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Create a stream
  ASSERT_EQ(conn.sendHeaders(1, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);

  // Bring stream send window up to INT32_MAX by issuing a single increase
  // increment: INT32_MAX - initialWindow
  constexpr int32_t kMaxWindow = std::numeric_limits<int32_t>::max();
  const uint32_t initialWindow = conn.peerSettings().initialWindowSize;
  Http2Stream* stream = conn.getStream(1);
  ASSERT_NE(stream, nullptr);

  const uint32_t increment =
      static_cast<uint32_t>(static_cast<int64_t>(kMaxWindow) - static_cast<int64_t>(initialWindow));
  const ErrorCode incErr = stream->increaseSendWindow(increment);
  EXPECT_EQ(incErr, ErrorCode::NoError);

  // Now apply SETTINGS_INITIAL_WINDOW_SIZE = initialWindow + 1 which will
  // cause newWindow = kMaxWindow + 1 -> overflow and should return FlowControlError
  const uint32_t newInitial = initialWindow + 1U;
  std::array<std::byte, 6> entry = {std::byte{0x00},
                                    std::byte{0x04},  // SETTINGS_INITIAL_WINDOW_SIZE
                                    std::byte{static_cast<uint8_t>((newInitial >> 24) & 0xFF)},
                                    std::byte{static_cast<uint8_t>((newInitial >> 16) & 0xFF)},
                                    std::byte{static_cast<uint8_t>((newInitial >> 8) & 0xFF)},
                                    std::byte{static_cast<uint8_t>(newInitial & 0xFF)}};

  FrameHeader header;
  header.length = entry.size();
  header.type = FrameType::Settings;
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, entry);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FlowControlError);
  EXPECT_TRUE(conn.hasPendingOutput());
  const auto out = conn.getPendingOutput();
  ASSERT_GE(out.size(), FrameHeader::kSize);
  bool foundGoAway2 = false;
  std::size_t pos2 = 0;
  while (pos2 + FrameHeader::kSize <= out.size()) {
    FrameHeader fh = ParseFrameHeader(out.subspan(pos2, FrameHeader::kSize));
    if (fh.type == FrameType::GoAway) {
      foundGoAway2 = true;
      break;
    }
    pos2 += FrameHeader::kSize + fh.length;
  }
  EXPECT_TRUE(foundGoAway2);
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

TEST(Http2Connection, WindowUpdateStreamOverflowSendsRstStream) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Create a stream
  ASSERT_EQ(conn.sendHeaders(1, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);

  // Bring stream send window up to INT32_MAX by issuing a single increase
  // increment: INT32_MAX - initialWindow
  constexpr int32_t kMaxWindow = std::numeric_limits<int32_t>::max();
  const uint32_t initialWindow = conn.peerSettings().initialWindowSize;
  Http2Stream* stream = conn.getStream(1);
  ASSERT_NE(stream, nullptr);

  const uint32_t increment =
      static_cast<uint32_t>(static_cast<int64_t>(kMaxWindow) - static_cast<int64_t>(initialWindow));
  const ErrorCode incErr = stream->increaseSendWindow(increment);
  EXPECT_EQ(incErr, ErrorCode::NoError);

  // Now send a WINDOW_UPDATE for the stream with increment 1 which will overflow
  const uint32_t winInc = 1U;
  std::array<std::byte, 4> payload = {
      std::byte{static_cast<uint8_t>((winInc >> 24) & 0xFF)}, std::byte{static_cast<uint8_t>((winInc >> 16) & 0xFF)},
      std::byte{static_cast<uint8_t>((winInc >> 8) & 0xFF)}, std::byte{static_cast<uint8_t>(winInc & 0xFF)}};

  FrameHeader header;
  header.length = payload.size();
  header.type = FrameType::WindowUpdate;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);

  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::OutputReady);

  // Search pending output for an RST_STREAM frame with FlowControlError
  ASSERT_TRUE(conn.hasPendingOutput());
  const auto out = conn.getPendingOutput();
  ASSERT_GE(out.size(), FrameHeader::kSize);

  bool foundRst = false;
  std::size_t pos = 0;
  while (pos + FrameHeader::kSize <= out.size()) {
    FrameHeader fh = ParseFrameHeader(out.subspan(pos, FrameHeader::kSize));
    if (fh.type == FrameType::RstStream && fh.length == 4U) {
      RstStreamFrame rst;
      const auto payloadView = out.subspan(pos + FrameHeader::kSize, fh.length);
      ASSERT_EQ(ParseRstStreamFrame(fh, payloadView, rst), FrameParseResult::Ok);
      EXPECT_EQ(rst.errorCode, ErrorCode::FlowControlError);
      foundRst = true;
      break;
    }
    pos += FrameHeader::kSize + fh.length;
  }
  EXPECT_TRUE(foundRst);
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

TEST(Http2Connection, ContinuationForPrunedStreamIsInternalError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Create a stream by sending a HEADERS frame WITH endHeaders=false so CONTINUATION is expected.
  // Build a minimal HEADERS frame containing a 1-byte header block fragment and no END_HEADERS flag.
  std::array<std::byte, 1> hb_fragment = {std::byte{0x00}};
  RawBytes headersBuf;
  WriteFrame(headersBuf, FrameType::Headers, ComputeHeaderFrameFlags(false, false), 1,
             static_cast<uint32_t>(hb_fragment.size()));
  headersBuf.unchecked_append(hb_fragment);
  auto headersSpan =
      std::span<const std::byte>(reinterpret_cast<const std::byte*>(headersBuf.data()), headersBuf.size());
  auto resHdr = conn.processInput(headersSpan);
  ASSERT_NE(resHdr.action, Http2Connection::ProcessResult::Action::Error);

  // Close stream 1 so it becomes eligible for pruning.
  conn.sendRstStream(1, ErrorCode::Cancel);

  // Now close and prune many streams so that stream 1 is removed from _streams map.
  // We will create and close (kClosedStreamsMaxRetainedForTest + 2) streams to force pruning.
  const uint32_t streamCountToClose = kClosedStreamsMaxRetainedForTest + 2U;
  for (uint32_t idx = 0; idx < streamCountToClose; ++idx) {
    const uint32_t sid = 3U + (idx * 2U);  // odd client-initiated stream ids
    ASSERT_EQ(conn.sendHeaders(sid, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);
    conn.sendRstStream(sid, ErrorCode::Cancel);
  }

  // Now send a CONTINUATION frame for stream 1 (endHeaders = true) which should trigger
  // InternalError "Stream not found for CONTINUATION" because stream 1 has been pruned.
  RawBytes buffer;
  WriteContinuationFrame(buffer, 1, hb_fragment, true);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  auto res = conn.processInput(span);

  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::InternalError);
  EXPECT_TRUE(conn.hasPendingOutput());

  // Pending output should contain GOAWAY
  const auto out = conn.getPendingOutput();
  bool foundGoAway = false;
  std::size_t pos = 0;
  while (pos + FrameHeader::kSize <= out.size()) {
    FrameHeader fh = ParseFrameHeader(out.subspan(pos, FrameHeader::kSize));
    if (fh.type == FrameType::GoAway) {
      foundGoAway = true;
      break;
    }
    pos += FrameHeader::kSize + fh.length;
  }
  EXPECT_TRUE(foundGoAway);
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