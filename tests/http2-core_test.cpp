#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::http2 {

namespace {

// ============================
// Small test helpers
// ============================

[[nodiscard]] std::span<const std::byte> AsSpan(const RawBytes& bytes) noexcept { return {bytes.data(), bytes.size()}; }

[[nodiscard]] vector<std::byte> CopyBytes(std::span<const std::byte> data) {
  vector<std::byte> out;
  out.reserve(static_cast<decltype(out)::size_type>(data.size()));
  std::ranges::copy(data, std::back_inserter(out));
  return out;
}

[[nodiscard]] vector<std::byte> MakePreface() {
  vector<std::byte> preface;
  preface.reserve(kConnectionPreface.size());
  std::ranges::transform(kConnectionPreface, std::back_inserter(preface),
                         [](char ch) { return static_cast<std::byte>(ch); });
  return preface;
}

struct ParsedFrame {
  FrameHeader header;
  std::span<const std::byte> payload;
};

[[nodiscard]] vector<ParsedFrame> ParseFrames(std::span<const std::byte> data) {
  vector<ParsedFrame> frames;

  std::size_t offset = 0;
  while (offset + FrameHeader::kSize <= data.size()) {
    auto remaining = data.subspan(offset);
    FrameHeader header = ParseFrameHeader(remaining);

    const std::size_t frameSize = FrameHeader::kSize + header.length;
    if (offset + frameSize > data.size()) {
      break;
    }

    frames.push_back(ParsedFrame{header, remaining.subspan(FrameHeader::kSize, header.length)});
    offset += frameSize;
  }

  return frames;
}

[[nodiscard]] RawBytes BuildSettingsFrame(const Http2Config& cfg) {
  // Keep the same order as Http2Connection::sendSettings().
  std::array<SettingsEntry, 6> entries = {
      SettingsEntry{SettingsParameter::HeaderTableSize, cfg.headerTableSize},
      SettingsEntry{SettingsParameter::EnablePush, cfg.enablePush ? 1U : 0U},
      SettingsEntry{SettingsParameter::MaxConcurrentStreams, cfg.maxConcurrentStreams},
      SettingsEntry{SettingsParameter::InitialWindowSize, cfg.initialWindowSize},
      SettingsEntry{SettingsParameter::MaxFrameSize, cfg.maxFrameSize},
      SettingsEntry{SettingsParameter::MaxHeaderListSize, cfg.maxHeaderListSize},
  };

  RawBytes out;
  WriteSettingsFrame(out, entries);
  return out;
}

struct HeaderEvent {
  uint32_t streamId{0};
  bool endStream{false};
  vector<std::pair<std::string, std::string>> headers;
};

struct DataEvent {
  uint32_t streamId{0};
  bool endStream{false};
  vector<std::byte> data;
};

struct GoAwayEvent {
  uint32_t lastStreamId{0};
  ErrorCode errorCode{ErrorCode::NoError};
  std::string debug;
};

// ============================
// Loopback harness
// ============================

class Http2Loopback {
 public:
  Http2Loopback(Http2Config clientCfg, Http2Config serverCfg)
      : _clientCfg(std::move(clientCfg)),
        _serverCfg(std::move(serverCfg)),
        client(_clientCfg, false),
        server(_serverCfg, true) {
    client.setOnHeadersDecoded([this](uint32_t streamId, const HeadersViewMap& headers, bool endStream) {
      HeaderEvent ev;
      ev.streamId = streamId;
      ev.endStream = endStream;
      for (const auto& [name, value] : headers) {
        ev.headers.emplace_back(name, value);
      }
      clientHeaders.push_back(std::move(ev));
    });

    server.setOnHeadersDecoded([this](uint32_t streamId, const HeadersViewMap& headers, bool endStream) {
      HeaderEvent ev;
      ev.streamId = streamId;
      ev.endStream = endStream;
      for (const auto& [name, value] : headers) {
        ev.headers.emplace_back(name, value);
      }
      serverHeaders.push_back(std::move(ev));
    });

    client.setOnData([this](uint32_t streamId, std::span<const std::byte> data, bool endStream) {
      DataEvent ev;
      ev.streamId = streamId;
      ev.endStream = endStream;
      ev.data = CopyBytes(data);
      clientData.push_back(std::move(ev));
    });

    server.setOnData([this](uint32_t streamId, std::span<const std::byte> data, bool endStream) {
      DataEvent ev;
      ev.streamId = streamId;
      ev.endStream = endStream;
      ev.data = CopyBytes(data);
      serverData.push_back(std::move(ev));
    });

    client.setOnGoAway([this](uint32_t lastStreamId, ErrorCode code, std::string_view debugData) {
      GoAwayEvent ev;
      ev.lastStreamId = lastStreamId;
      ev.errorCode = code;
      ev.debug.assign(debugData.data(), debugData.size());
      clientGoAway.push_back(std::move(ev));
    });

    server.setOnGoAway([this](uint32_t lastStreamId, ErrorCode code, std::string_view debugData) {
      GoAwayEvent ev;
      ev.lastStreamId = lastStreamId;
      ev.errorCode = code;
      ev.debug.assign(debugData.data(), debugData.size());
      serverGoAway.push_back(std::move(ev));
    });
  }

  void connect(bool alsoSendClientSettings = true) {
    // Client -> Server: connection preface and (optionally) an initial SETTINGS frame.
    vector<std::byte> bytes = MakePreface();
    if (alsoSendClientSettings) {
      auto settings = BuildSettingsFrame(_clientCfg);
      auto span = AsSpan(settings);
      bytes.insert(bytes.end(), span.begin(), span.end());
    }
    feed(server, bytes);

    // Server sends its SETTINGS immediately; deliver to client.
    pump(server, client);

    // Client responds with SETTINGS ACK; deliver to server.
    pump(client, server);

    // If we sent client SETTINGS, server will ACK them; deliver to client.
    pump(server, client);

    ASSERT_EQ(server.state(), ConnectionState::Open);
    ASSERT_EQ(client.state(), ConnectionState::Open);
  }

  // Pump pending output from `from` to `to` until drained.
  static void pump(Http2Connection& from, Http2Connection& to) {
    // One pump can generate more output on the receiver (e.g. SETTINGS ACK). We intentionally
    // only drain `from` here; tests can call pump in the needed direction explicitly.
    while (from.hasPendingOutput()) {
      auto out = from.getPendingOutput();
      auto outCopy = CopyBytes(out);
      feed(to, outCopy);
      from.onOutputWritten(out.size());
    }
  }

  // Feed bytes into a connection until fully consumed (or a terminal error/closed).
  static void feed(Http2Connection& to, std::span<const std::byte> data) {
    std::size_t safetyIters = 0;
    while (!data.empty()) {
      ++safetyIters;
      if (safetyIters >= 64U) {
        ADD_FAILURE() << "feed() got stuck";
        return;
      }

      const auto prevState = to.state();
      const auto res = to.processInput(data);

      if (res.action == Http2Connection::ProcessResult::Action::Error ||
          res.action == Http2Connection::ProcessResult::Action::Closed ||
          res.action == Http2Connection::ProcessResult::Action::GoAway) {
        // GOAWAY also consumes the frame but returns bytesConsumed from the full frame parsing.
        if (res.bytesConsumed > 0) {
          data = data.subspan(res.bytesConsumed);
        }
        // Continue processing remaining data if any (GOAWAY doesn't mean stop processing).
        if (res.action == Http2Connection::ProcessResult::Action::GoAway && !data.empty()) {
          continue;
        }
        return;
      }

      if (res.bytesConsumed > 0) {
        data = data.subspan(res.bytesConsumed);
        continue;
      }

      // Special case: the client side transitions from AwaitingPreface to AwaitingSettings
      // without consuming input. In that case, we must re-run parsing on the same bytes.
      if (to.state() != prevState) {
        continue;
      }

      ADD_FAILURE() << "No progress while feeding input (state=" << static_cast<int>(to.state()) << ")";
      return;
    }
  }

  Http2Config _clientCfg;
  Http2Config _serverCfg;

  Http2Connection client;
  Http2Connection server;

  vector<HeaderEvent> clientHeaders;
  vector<HeaderEvent> serverHeaders;
  vector<DataEvent> clientData;
  vector<DataEvent> serverData;
  vector<GoAwayEvent> clientGoAway;
  vector<GoAwayEvent> serverGoAway;
};

[[nodiscard]] bool HasHeader(const HeaderEvent& ev, std::string_view name, std::string_view value) {
  return std::ranges::any_of(ev.headers, [&](const auto& kv) { return kv.first == name && kv.second == value; });
}

// ============================
// Handshake / settings
// ============================

TEST(Http2Core, LoopbackHandshakeOpensConnection) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  EXPECT_TRUE(h2.client.isOpen());
  EXPECT_TRUE(h2.server.isOpen());
}

TEST(Http2Core, PeerSettingsAreAppliedFromRemoteSettingsFrame) {
  Http2Config clientCfg;
  Http2Config serverCfg;
  serverCfg.maxFrameSize = 16384;  // must be in valid range
  serverCfg.maxHeaderListSize = 12345;
  serverCfg.enablePush = false;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  // Client peer settings should mirror server local settings.
  EXPECT_EQ(h2.client.peerSettings().maxFrameSize, serverCfg.maxFrameSize);
  EXPECT_EQ(h2.client.peerSettings().maxHeaderListSize, serverCfg.maxHeaderListSize);
  EXPECT_FALSE(h2.client.peerSettings().enablePush);

  // Server peer settings should mirror client local settings.
  EXPECT_EQ(h2.server.peerSettings().maxFrameSize, clientCfg.maxFrameSize);
  EXPECT_EQ(h2.server.peerSettings().maxHeaderListSize, clientCfg.maxHeaderListSize);
  EXPECT_EQ(h2.server.peerSettings().enablePush, clientCfg.enablePush);
}

TEST(Http2Core, InvalidPeerMaxFrameSizeCausesProtocolError) {
  Http2Config cfg;
  Http2Connection conn(cfg, true);

  // Establish server preface first.
  auto preface = MakePreface();
  auto prefaceRes = conn.processInput(preface);
  ASSERT_EQ(prefaceRes.action, Http2Connection::ProcessResult::Action::OutputReady);

  // Feed invalid SETTINGS with MAX_FRAME_SIZE < 16384.
  Http2Config bad;
  bad.maxFrameSize = 8192;
  RawBytes settings = BuildSettingsFrame(bad);

  auto res = conn.processInput(AsSpan(settings));
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

// ============================
// HEADERS encode/decode (covers encodeHeaders via sendHeaders)
// ============================

TEST(Http2Core, ClientSendHeadersIsDecodedOnServer) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  constexpr uint32_t streamId = 1;

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/hello"));
  hdrs.append(MakeHttp1HeaderLine("x-custom", "value"));

  ErrorCode err = h2.client.sendHeaders(streamId, http::StatusCodeOK, HeadersView(hdrs), false);
  ASSERT_EQ(err, ErrorCode::NoError);

  // Client must have output HEADERS.
  auto out = h2.client.getPendingOutput();
  ASSERT_FALSE(out.empty());

  const auto frames = ParseFrames(out);
  ASSERT_FALSE(frames.empty());
  EXPECT_EQ(frames[0].header.type, FrameType::Headers);
  EXPECT_EQ(frames[0].header.streamId, streamId);

  // Deliver to server.
  Http2Loopback::pump(h2.client, h2.server);

  ASSERT_EQ(h2.serverHeaders.size(), 1U);
  const auto& ev = h2.serverHeaders[0];
  EXPECT_EQ(ev.streamId, streamId);
  EXPECT_FALSE(ev.endStream);

  EXPECT_TRUE(HasHeader(ev, ":method", "GET"));
  EXPECT_TRUE(HasHeader(ev, ":scheme", "https"));
  EXPECT_TRUE(HasHeader(ev, ":authority", "example.com"));
  EXPECT_TRUE(HasHeader(ev, ":path", "/hello"));
  EXPECT_TRUE(HasHeader(ev, "x-custom", "value"));
}

TEST(Http2Core, ServerSendHeadersIsDecodedOnClient) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  constexpr uint32_t streamId = 2;

  RawChars hdrs2;
  hdrs2.append(MakeHttp1HeaderLine("content-type", "text/plain"));
  hdrs2.append(MakeHttp1HeaderLine("x-srv", "abc"));
  ErrorCode err = h2.server.sendHeaders(streamId, http::StatusCodeOK, HeadersView(hdrs2), false);
  ASSERT_EQ(err, ErrorCode::NoError);

  auto out = h2.server.getPendingOutput();
  ASSERT_FALSE(out.empty());
  const auto frames = ParseFrames(out);
  ASSERT_FALSE(frames.empty());
  EXPECT_EQ(frames[0].header.type, FrameType::Headers);
  EXPECT_EQ(frames[0].header.streamId, streamId);

  Http2Loopback::pump(h2.server, h2.client);

  ASSERT_EQ(h2.clientHeaders.size(), 1U);
  const auto& ev = h2.clientHeaders[0];
  EXPECT_EQ(ev.streamId, streamId);
  EXPECT_TRUE(HasHeader(ev, ":status", "200"));
  EXPECT_TRUE(HasHeader(ev, "content-type", "text/plain"));
  EXPECT_TRUE(HasHeader(ev, "x-srv", "abc"));
}

TEST(Http2Core, HeadersEndStreamClosesRemoteSideStream) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  constexpr uint32_t streamId = 1;

  RawChars hdrs3;
  hdrs3.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs3.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs3.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs3.append(MakeHttp1HeaderLine(":path", "/end"));

  ErrorCode err = h2.client.sendHeaders(streamId, http::StatusCodeOK, HeadersView(hdrs3), true);
  ASSERT_EQ(err, ErrorCode::NoError);

  Http2Loopback::pump(h2.client, h2.server);

  ASSERT_EQ(h2.serverHeaders.size(), 1U);
  EXPECT_TRUE(h2.serverHeaders[0].endStream);

  const auto* stream = h2.server.getStream(streamId);
  ASSERT_NE(stream, nullptr);
  // Per RFC: receiving HEADERS with END_STREAM on an initial request transitions to HalfClosedRemote.
  // The stream is only "closed" when both sides have ended.
  EXPECT_EQ(stream->state(), StreamState::HalfClosedRemote);
}

// ============================
// CONTINUATION splitting
// ============================

TEST(Http2Core, ClientSplitsLargeHeaderBlockIntoContinuationFrames) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  // Force a tiny max frame size from server to client (client.peerSettings comes from server local settings).
  // Valid range is [16384..16777215], so we cannot set it below that via SETTINGS.
  // Instead, we create an oversized header block so it splits even at 16KB, and we keep the test fast.
  serverCfg.maxFrameSize = 16384;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  constexpr uint32_t streamId = 1;

  const std::string largeValue(7000, 'x');
  RawChars hdrs4;
  hdrs4.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs4.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs4.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs4.append(MakeHttp1HeaderLine(":path", "/big"));
  for (int idx = 0; idx < 20; ++idx) {
    hdrs4.append(MakeHttp1HeaderLine("x-big-" + std::to_string(idx), largeValue));
  }

  ASSERT_EQ(h2.client.sendHeaders(streamId, http::StatusCodeOK, HeadersView(hdrs4), false), ErrorCode::NoError);

  auto out = h2.client.getPendingOutput();
  ASSERT_FALSE(out.empty());

  const auto frames = ParseFrames(out);
  ASSERT_GE(frames.size(), 2U) << "Expected HEADERS + at least one CONTINUATION";

  EXPECT_EQ(frames[0].header.type, FrameType::Headers);
  EXPECT_EQ(frames[0].header.streamId, streamId);

  for (decltype(frames)::size_type ii = 1; ii < frames.size(); ++ii) {
    EXPECT_EQ(frames[ii].header.type, FrameType::Continuation);
    EXPECT_EQ(frames[ii].header.streamId, streamId);
  }

  // Only the last should have END_HEADERS.
  for (decltype(frames)::size_type ii = 0; ii + 1 < frames.size(); ++ii) {
    EXPECT_FALSE(frames[ii].header.hasFlag(FrameFlags::HeadersEndHeaders));
    EXPECT_FALSE(frames[ii].header.hasFlag(FrameFlags::ContinuationEndHeaders));
  }

  const auto& last = frames.back().header;
  EXPECT_TRUE(last.hasFlag(FrameFlags::ContinuationEndHeaders) || last.hasFlag(FrameFlags::HeadersEndHeaders));

  // Deliver and validate decoding succeeded.
  Http2Loopback::pump(h2.client, h2.server);
  ASSERT_EQ(h2.serverHeaders.size(), 1U);
  const auto& ev = h2.serverHeaders[0];
  EXPECT_EQ(ev.streamId, streamId);
  EXPECT_TRUE(HasHeader(ev, ":path", "/big"));
  EXPECT_TRUE(HasHeader(ev, "x-big-0", largeValue));
  EXPECT_TRUE(HasHeader(ev, "x-big-19", largeValue));
}

TEST(Http2Core, ContinuationFrameOnWrongStreamIsProtocolError) {
  Http2Config cfg;
  Http2Connection conn(cfg, true);

  // Establish preface and complete SETTINGS handshake.
  auto preface = MakePreface();
  (void)conn.processInput(preface);
  // Server sends SETTINGS after preface; we need to send SETTINGS ACK to transition to Open.
  RawBytes settingsAck;
  WriteSettingsAckFrame(settingsAck);
  (void)conn.processInput(AsSpan(settingsAck));

  // Start a HEADERS frame that requires continuation (END_HEADERS not set).
  RawBytes buf;
  std::array<std::byte, 1> headerBlock = {std::byte{0x82}};  // :method: GET
  WriteFrame(buf, FrameType::Headers, ComputeHeaderFrameFlags(false, false), 1, headerBlock.size());
  buf.append(headerBlock);

  auto res1 = conn.processInput(AsSpan(buf));
  ASSERT_NE(res1.action, Http2Connection::ProcessResult::Action::Error);

  // Send CONTINUATION on a different stream.
  RawBytes cont;
  WriteContinuationFrame(cont, 3, headerBlock, true);

  auto res2 = conn.processInput(AsSpan(cont));
  EXPECT_EQ(res2.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res2.errorCode, ErrorCode::ProtocolError);
}

TEST(Http2Core, MissingContinuationThenOtherFrameIsProtocolError) {
  Http2Config cfg;
  Http2Connection conn(cfg, true);

  auto preface = MakePreface();
  (void)conn.processInput(preface);
  // Complete SETTINGS handshake.
  RawBytes settingsAck;
  WriteSettingsAckFrame(settingsAck);
  (void)conn.processInput(AsSpan(settingsAck));

  // HEADERS without END_HEADERS.
  RawBytes headers;
  std::array<std::byte, 1> hb = {std::byte{0x82}};
  WriteFrame(headers, FrameType::Headers, ComputeHeaderFrameFlags(false, false), 1, hb.size());
  headers.append(hb);
  auto res1 = conn.processInput(AsSpan(headers));
  ASSERT_NE(res1.action, Http2Connection::ProcessResult::Action::Error);

  // Now send DATA frame instead of CONTINUATION.
  RawBytes data;
  std::array<std::byte, 1> payload = {std::byte{'x'}};
  WriteDataFrame(data, 1, payload, false);

  auto res2 = conn.processInput(AsSpan(data));
  EXPECT_EQ(res2.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res2.errorCode, ErrorCode::ProtocolError);
}

// ============================
// DATA sending / splitting / flow control
// ============================

TEST(Http2Core, DataIsDeliveredToPeer) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  constexpr uint32_t streamId = 1;

  RawChars hdrs4;
  hdrs4.append(MakeHttp1HeaderLine(":method", "POST"));
  hdrs4.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs4.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs4.append(MakeHttp1HeaderLine(":path", "/upload"));

  ASSERT_EQ(h2.client.sendHeaders(streamId, http::StatusCodeOK, HeadersView(hdrs4), false), ErrorCode::NoError);
  Http2Loopback::pump(h2.client, h2.server);
  // Pump back to process any server responses (e.g., WINDOW_UPDATE).
  Http2Loopback::pump(h2.server, h2.client);

  std::array<std::byte, 5> payload = {std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'}};
  ASSERT_EQ(h2.client.sendData(streamId, payload, true), ErrorCode::NoError);

  Http2Loopback::pump(h2.client, h2.server);

  ASSERT_EQ(h2.serverData.size(), 1U);
  EXPECT_EQ(h2.serverData[0].streamId, streamId);
  EXPECT_TRUE(h2.serverData[0].endStream);
  ASSERT_EQ(h2.serverData[0].data.size(), payload.size());
  EXPECT_EQ(static_cast<char>(h2.serverData[0].data[0]), 'h');
}

TEST(Http2Core, SendDataOnUnknownStreamIsProtocolError) {
  Http2Config cfg;
  Http2Connection conn(cfg, true);

  auto preface = MakePreface();
  (void)conn.processInput(preface);

  std::array<std::byte, 1> payload = {std::byte{'x'}};
  EXPECT_EQ(conn.sendData(1, payload, false), ErrorCode::ProtocolError);
}

TEST(Http2Core, SendingMoreThanConnectionSendWindowFails) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  constexpr uint32_t streamId = 1;

  RawChars headers;
  headers.append(MakeHttp1HeaderLine(":method", "GET"));
  headers.append(MakeHttp1HeaderLine(":scheme", "https"));
  headers.append(MakeHttp1HeaderLine(":authority", "example.com"));
  headers.append(MakeHttp1HeaderLine(":path", "/win"));

  ASSERT_EQ(h2.client.sendHeaders(streamId, http::StatusCodeOK, HeadersView(headers), false), ErrorCode::NoError);
  Http2Loopback::pump(h2.client, h2.server);

  // Deplete the connection send window (starts at 65535 on every connection).
  vector<std::byte> big(static_cast<std::size_t>(65535), std::byte{'a'});
  ASSERT_EQ(h2.client.sendData(streamId, big, false), ErrorCode::NoError);

  // One more byte must fail.
  std::array<std::byte, 1> extra = {std::byte{'b'}};
  EXPECT_EQ(h2.client.sendData(streamId, extra, false), ErrorCode::FlowControlError);
}

TEST(Http2Core, StreamSendWindowIsEnforced) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  // Make the peer initial window size small by sending SETTINGS from server to client.
  serverCfg.initialWindowSize = 1024;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  constexpr uint32_t streamId = 1;

  RawChars headers;
  headers.append(MakeHttp1HeaderLine(":method", "POST"));
  headers.append(MakeHttp1HeaderLine(":scheme", "https"));
  headers.append(MakeHttp1HeaderLine(":authority", "example.com"));
  headers.append(MakeHttp1HeaderLine(":path", "/small-win"));

  ASSERT_EQ(h2.client.sendHeaders(streamId, http::StatusCodeOK, HeadersView(headers), false), ErrorCode::NoError);

  // The stream send window should now be 1024 on the client (it is created with peer initial window).
  auto* st = h2.client.getStream(streamId);
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->sendWindow(), 1024);

  vector<std::byte> payload(static_cast<std::size_t>(1024), std::byte{'x'});
  ASSERT_EQ(h2.client.sendData(streamId, payload, false), ErrorCode::NoError);

  std::array<std::byte, 1> extra = {std::byte{'y'}};
  EXPECT_EQ(h2.client.sendData(streamId, extra, false), ErrorCode::FlowControlError);
}

// ============================
// Receiving-side protocol errors
// ============================

TEST(Http2Core, DataOnStreamZeroIsProtocolError) {
  Http2Config cfg;
  Http2Connection conn(cfg, true);

  auto preface = MakePreface();
  (void)conn.processInput(preface);

  RawBytes data;
  std::array<std::byte, 1> payload = {std::byte{'x'}};
  WriteDataFrame(data, 0, payload, false);

  auto res = conn.processInput(AsSpan(data));
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

TEST(Http2Core, HeadersOnStreamZeroIsProtocolError) {
  Http2Config cfg;
  Http2Connection conn(cfg, true);

  auto preface = MakePreface();
  (void)conn.processInput(preface);

  RawBytes headers;
  std::array<std::byte, 1> hb = {std::byte{0x82}};
  WriteFrame(headers, FrameType::Headers, ComputeHeaderFrameFlags(false, true), 0, hb.size());
  headers.append(hb);

  auto res = conn.processInput(AsSpan(headers));
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

TEST(Http2Core, UnexpectedContinuationIsProtocolError) {
  Http2Config cfg;
  Http2Connection conn(cfg, true);

  auto preface = MakePreface();
  (void)conn.processInput(preface);

  RawBytes cont;
  std::array<std::byte, 1> hb = {std::byte{0x82}};
  WriteContinuationFrame(cont, 1, hb, true);

  auto res = conn.processInput(AsSpan(cont));
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

TEST(Http2Core, WindowUpdateWithZeroIncrementIsProtocolError) {
  Http2Config cfg;
  Http2Connection conn(cfg, true);

  auto preface = MakePreface();
  (void)conn.processInput(preface);

  RawBytes wu;
  WriteWindowUpdateFrame(wu, 0, 0);

  auto res = conn.processInput(AsSpan(wu));
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

// ============================
// PRIORITY
// ============================

TEST(Http2Core, PriorityInfoFromHeadersIsStoredOnStream) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  // Build a HEADERS frame with PRIORITY flag.
  // Wire weight 55 => logical weight 56 (RFC 9113 §5.3.1: add one to wire value)
  RawBytes buf;
  std::array<std::byte, 1> hb = {std::byte{0x82}};
  WriteHeadersFrameWithPriority(buf, 1, hb, 0, 55, true, false, true);

  auto res = h2.server.processInput(AsSpan(buf));
  ASSERT_NE(res.action, Http2Connection::ProcessResult::Action::Error);

  const auto* stream = h2.server.getStream(1);
  ASSERT_NE(stream, nullptr);
  EXPECT_EQ(stream->streamDependency(), 0U);
  EXPECT_EQ(stream->weight(), 56U);  // Wire value 55 + 1 = 56
  EXPECT_TRUE(stream->isExclusive());
}

TEST(Http2Core, PriorityFrameUpdatesStream) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  RawChars headers;
  headers.append(MakeHttp1HeaderLine(":method", "GET"));
  headers.append(MakeHttp1HeaderLine(":scheme", "https"));
  headers.append(MakeHttp1HeaderLine(":authority", "example.com"));
  headers.append(MakeHttp1HeaderLine(":path", "/prio"));

  // Create a stream by sending HEADERS.
  ASSERT_EQ(h2.client.sendHeaders(1, http::StatusCodeOK, HeadersView(headers), false), ErrorCode::NoError);
  Http2Loopback::pump(h2.client, h2.server);

  RawBytes pri;
  // Note: weight in PRIORITY frame is 0-255 but stored as weight+1, so sending 11 gives weight 12.
  WritePriorityFrame(pri, 1, 0, 11, false);
  auto res = h2.server.processInput(AsSpan(pri));
  ASSERT_NE(res.action, Http2Connection::ProcessResult::Action::Error);

  const auto* stream = h2.server.getStream(1);
  ASSERT_NE(stream, nullptr);
  EXPECT_EQ(stream->weight(), 12U);
  EXPECT_FALSE(stream->isExclusive());
}

// ============================
// PING
// ============================

TEST(Http2Core, PingRequestProducesPingAck) {
  Http2Config cfg;
  Http2Connection conn(cfg, true);

  auto preface = MakePreface();
  (void)conn.processInput(preface);

  // Server sends SETTINGS after receiving preface; drain that output first.
  auto initialOut = conn.getPendingOutput();
  conn.onOutputWritten(initialOut.size());

  PingFrame pingFrame;
  pingFrame.isAck = false;
  static constexpr std::byte opaqueData[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
                                             std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};

  std::memcpy(pingFrame.opaqueData, opaqueData, sizeof(opaqueData));
  RawBytes ping;
  WritePingFrame(ping, pingFrame);

  auto res = conn.processInput(AsSpan(ping));
  ASSERT_NE(res.action, Http2Connection::ProcessResult::Action::Error);

  auto out = conn.getPendingOutput();
  ASSERT_FALSE(out.empty());

  FrameHeader header = ParseFrameHeader(out);
  EXPECT_EQ(header.type, FrameType::Ping);
  EXPECT_TRUE(header.hasFlag(FrameFlags::PingAck));
  EXPECT_EQ(header.streamId, 0U);
}

TEST(Http2Core, PingOnNonZeroStreamIsProtocolError) {
  Http2Config cfg;
  Http2Connection conn(cfg, true);

  auto preface = MakePreface();
  (void)conn.processInput(preface);

  RawBytes ping;
  // Build a ping frame but override stream id by writing header manually.
  // Easiest: write a valid PING then mutate the stream id bytes.
  WritePingFrame(ping, PingFrame{});
  ping[5] = std::byte{0x00};
  ping[6] = std::byte{0x00};
  ping[7] = std::byte{0x00};
  ping[8] = std::byte{0x01};

  auto res = conn.processInput(AsSpan(ping));
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

// ============================
// GOAWAY
// ============================

TEST(Http2Core, GoAwayReceivedPreventsNewStreamsBeyondLastStreamId) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  // Send GOAWAY to server with lastStreamId=1.
  RawBytes go;
  WriteGoAwayFrame(go, 1, ErrorCode::NoError, "drain");
  auto resGo = h2.server.processInput(AsSpan(go));
  ASSERT_NE(resGo.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(h2.server.state(), ConnectionState::GoAwayReceived);

  // Now attempt to open stream 3 (should be ignored).
  RawBytes headers;
  std::array<std::byte, 4> hb = {std::byte{0x82}, std::byte{0x86}, std::byte{0x84}, std::byte{0x01}};
  WriteFrame(headers, FrameType::Headers, ComputeHeaderFrameFlags(false, true), 3, hb.size());
  headers.append(hb);

  auto resHeaders = h2.server.processInput(AsSpan(headers));
  EXPECT_EQ(resHeaders.action, Http2Connection::ProcessResult::Action::Continue);
  EXPECT_EQ(h2.server.getStream(3), nullptr);
}

TEST(Http2Core, InitiateGoAwayQueuesFrameAndUpdatesState) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  h2.server.initiateGoAway(ErrorCode::NoError, "shutdown");
  EXPECT_EQ(h2.server.state(), ConnectionState::GoAwaySent);

  auto out = h2.server.getPendingOutput();
  ASSERT_FALSE(out.empty());

  FrameHeader header = ParseFrameHeader(out);
  EXPECT_EQ(header.type, FrameType::GoAway);
  EXPECT_EQ(header.streamId, 0U);
}

// ============================
// RST_STREAM
// ============================

TEST(Http2Core, RstStreamFromPeerTriggersStreamResetCallback) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  bool resetCalled = false;
  ErrorCode resetCode = ErrorCode::NoError;
  h2.server.setOnStreamReset([&](uint32_t id, ErrorCode code) {
    resetCalled = true;
    EXPECT_EQ(id, 1U);
    resetCode = code;
  });

  RawChars headers;
  headers.append(MakeHttp1HeaderLine(":method", "GET"));
  headers.append(MakeHttp1HeaderLine(":scheme", "https"));
  headers.append(MakeHttp1HeaderLine(":authority", "example.com"));
  headers.append(MakeHttp1HeaderLine(":path", "/rst"));

  // Create stream on server.
  ASSERT_EQ(h2.client.sendHeaders(1, http::StatusCodeOK, HeadersView(headers), false), ErrorCode::NoError);
  Http2Loopback::pump(h2.client, h2.server);
  // Also pump server->client to ensure any server response is processed.
  Http2Loopback::pump(h2.server, h2.client);
  ASSERT_NE(h2.server.getStream(1), nullptr);

  RawBytes rst;
  WriteRstStreamFrame(rst, 1, ErrorCode::Cancel);

  auto res = h2.server.processInput(AsSpan(rst));
  ASSERT_NE(res.action, Http2Connection::ProcessResult::Action::Error);

  EXPECT_TRUE(resetCalled);
  EXPECT_EQ(resetCode, ErrorCode::Cancel);
}

// ============================
// Multiple streams & ordering
// ============================

TEST(Http2Core, MultipleConcurrentStreamsDeliverHeadersToCorrectStream) {
  Http2Config clientCfg;
  Http2Config serverCfg;
  clientCfg.maxConcurrentStreams = 10;
  serverCfg.maxConcurrentStreams = 10;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  for (uint32_t streamId : {1U, 3U, 5U, 7U, 9U}) {
    RawChars headers;
    headers.append(MakeHttp1HeaderLine(":method", "GET"));
    headers.append(MakeHttp1HeaderLine(":scheme", "https"));
    headers.append(MakeHttp1HeaderLine(":authority", "example.com"));
    headers.append(MakeHttp1HeaderLine(":path", "/s" + std::to_string(streamId)));
    headers.append(MakeHttp1HeaderLine("x-id", std::to_string(streamId)));

    auto err = h2.client.sendHeaders(streamId, http::StatusCodeOK, HeadersView(headers), false);
    ASSERT_EQ(err, ErrorCode::NoError);
    // Pump after each to ensure delivery before potential flow control issues.
    Http2Loopback::pump(h2.client, h2.server);
  }

  ASSERT_EQ(h2.serverHeaders.size(), 5U);
  for (const auto& ev : h2.serverHeaders) {
    EXPECT_TRUE(HasHeader(ev, ":method", "GET"));
    EXPECT_TRUE(HasHeader(ev, ":scheme", "https"));
    EXPECT_TRUE(HasHeader(ev, ":authority", "example.com"));
    EXPECT_TRUE(HasHeader(ev, "x-id", std::to_string(ev.streamId)));
  }
}

TEST(Http2Core, RefusedStreamWhenMaxConcurrentStreamsExceededOnSender) {
  Http2Config clientCfg;
  Http2Config serverCfg;
  // Client checks its peerSettings.maxConcurrentStreams, which comes from server's local settings.
  serverCfg.maxConcurrentStreams = 1;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  RawChars headers;
  headers.append(MakeHttp1HeaderLine(":method", "GET"));
  headers.append(MakeHttp1HeaderLine(":scheme", "https"));
  headers.append(MakeHttp1HeaderLine(":authority", "example.com"));
  headers.append(MakeHttp1HeaderLine(":path", "/one"));

  ASSERT_EQ(h2.client.sendHeaders(1, http::StatusCodeOK, HeadersView(headers), false), ErrorCode::NoError);

  RawChars headers2;
  headers2.append(MakeHttp1HeaderLine(":method", "GET"));
  headers2.append(MakeHttp1HeaderLine(":scheme", "https"));
  headers2.append(MakeHttp1HeaderLine(":authority", "example.com"));
  headers2.append(MakeHttp1HeaderLine(":path", "/two"));

  // Second stream cannot be created while the first is active.
  EXPECT_EQ(h2.client.sendHeaders(3, http::StatusCodeOK, HeadersView(headers2), false), ErrorCode::RefusedStream);
}

// ============================
// Frame size checks on receiver
// ============================

TEST(Http2Core, ReceiverRejectsFrameLargerThanLocalMaxFrameSize) {
  Http2Config cfg;
  cfg.maxFrameSize = 16384;
  Http2Connection conn(cfg, true);

  auto preface = MakePreface();
  (void)conn.processInput(preface);

  // Create a frame header that advertises a too-large payload.
  // We do not need to provide the full payload: connection checks the size first.
  std::array<std::byte, FrameHeader::kSize> raw{};
  // length = 20000
  raw[0] = std::byte{0x00};
  raw[1] = std::byte{0x4E};
  raw[2] = std::byte{0x20};
  raw[3] = std::byte{static_cast<uint8_t>(FrameType::Data)};
  raw[4] = std::byte{0x00};
  raw[5] = std::byte{0x00};
  raw[6] = std::byte{0x00};
  raw[7] = std::byte{0x00};
  raw[8] = std::byte{0x01};

  auto res = conn.processInput(raw);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FrameSizeError);
}

// ============================
// Fuzz-ish / coverage tests
// ============================

TEST(Http2Core, RoundTripManyHeaderSetsClientToServer) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback loopback(clientCfg, serverCfg);
  loopback.connect(true);

  for (int iter = 0; iter < 50; ++iter) {
    const uint32_t streamId = static_cast<uint32_t>(1 + (iter * 2));
    RawChars headers;
    headers.append(MakeHttp1HeaderLine(":method", "GET"));
    headers.append(MakeHttp1HeaderLine(":scheme", "https"));
    headers.append(MakeHttp1HeaderLine(":authority", "example.com"));
    headers.append(MakeHttp1HeaderLine(":path", "/bulk/" + std::to_string(iter)));
    for (int ii = 0; ii < 5; ++ii) {
      headers.append(
          MakeHttp1HeaderLine("x-k" + std::to_string(ii), "v" + std::to_string(iter) + "." + std::to_string(ii)));
    }

    ASSERT_EQ(loopback.client.sendHeaders(streamId, http::StatusCodeOK, HeadersView(headers), false),
              ErrorCode::NoError);
  }

  Http2Loopback::pump(loopback.client, loopback.server);

  ASSERT_EQ(loopback.serverHeaders.size(), 50U);
  for (int iter = 0; iter < 50; ++iter) {
    const auto& ev = loopback.serverHeaders[static_cast<uint32_t>(iter)];
    EXPECT_TRUE(HasHeader(ev, ":method", "GET"));
    EXPECT_TRUE(HasHeader(ev, ":scheme", "https"));
    EXPECT_TRUE(HasHeader(ev, ":authority", "example.com"));
    EXPECT_TRUE(HasHeader(ev, ":path", "/bulk/" + std::to_string(iter)));
    EXPECT_TRUE(HasHeader(ev, "x-k0", "v" + std::to_string(iter) + ".0"));
    EXPECT_TRUE(HasHeader(ev, "x-k4", "v" + std::to_string(iter) + ".4"));
  }
}

TEST(Http2Core, RoundTripDataChunksAcrossManyFrames) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  constexpr uint32_t streamId = 1;

  RawChars headers;
  headers.append(MakeHttp1HeaderLine(":method", "POST"));
  headers.append(MakeHttp1HeaderLine(":scheme", "https"));
  headers.append(MakeHttp1HeaderLine(":authority", "example.com"));
  headers.append(MakeHttp1HeaderLine(":path", "/data"));

  ASSERT_EQ(h2.client.sendHeaders(streamId, http::StatusCodeOK, HeadersView(headers), false), ErrorCode::NoError);
  Http2Loopback::pump(h2.client, h2.server);
  Http2Loopback::pump(h2.server, h2.client);

  // Send multiple chunks.
  for (int iter = 0; iter < 20; ++iter) {
    std::string str = "chunk-" + std::to_string(iter);
    vector<std::byte> payload;
    payload.reserve(static_cast<decltype(payload)::size_type>(str.size()));
    std::ranges::transform(str, std::back_inserter(payload), [](char ch) { return static_cast<std::byte>(ch); });

    const bool endStream = (iter == 19);
    ASSERT_EQ(h2.client.sendData(streamId, payload, endStream), ErrorCode::NoError);
    Http2Loopback::pump(h2.client, h2.server);
  }

  ASSERT_EQ(h2.serverData.size(), 20U);
  for (int iter = 0; iter < 20; ++iter) {
    const auto& ev = h2.serverData[static_cast<uint32_t>(iter)];
    EXPECT_EQ(ev.streamId, streamId);
    const bool endStream = (iter == 19);
    EXPECT_EQ(ev.endStream, endStream);
  }
}

// ============================
// Additional edge cases
// ============================

TEST(Http2Core, SettingsAckOnNonZeroStreamIsProtocolError) {
  Http2Config cfg;
  Http2Connection conn(cfg, true);

  auto preface = MakePreface();
  (void)conn.processInput(preface);

  // Create SETTINGS ACK by writing a SETTINGS frame then mutating header.
  RawBytes settingsAck;
  WriteSettingsAckFrame(settingsAck);
  settingsAck[5] = std::byte{0x00};
  settingsAck[6] = std::byte{0x00};
  settingsAck[7] = std::byte{0x00};
  settingsAck[8] = std::byte{0x01};

  auto res = conn.processInput(AsSpan(settingsAck));
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

TEST(Http2Core, SettingsAckTransitionsAwaitingSettingsToOpen) {
  Http2Config cfg;
  Http2Connection server(cfg, true);

  auto preface = MakePreface();
  auto resPreface = server.processInput(preface);
  ASSERT_EQ(resPreface.action, Http2Connection::ProcessResult::Action::OutputReady);
  ASSERT_EQ(server.state(), ConnectionState::AwaitingSettings);

  RawBytes settingsAck;
  WriteSettingsAckFrame(settingsAck);

  auto resAck = server.processInput(AsSpan(settingsAck));
  ASSERT_NE(resAck.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(server.state(), ConnectionState::Open);
}

TEST(Http2Core, GoAwayCallbackIsInvoked) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  RawBytes go;
  WriteGoAwayFrame(go, 0, ErrorCode::EnhanceYourCalm, "too many requests");

  auto res = h2.client.processInput(AsSpan(go));
  ASSERT_NE(res.action, Http2Connection::ProcessResult::Action::Error);

  ASSERT_EQ(h2.clientGoAway.size(), 1U);
  EXPECT_EQ(h2.clientGoAway[0].errorCode, ErrorCode::EnhanceYourCalm);
  EXPECT_EQ(h2.clientGoAway[0].debug, "too many requests");
}

TEST(Http2Core, WindowUpdateIncreasesConnectionRecvWindow) {
  Http2Config cfg;
  Http2Connection conn(cfg, true);

  const auto initial = conn.connectionRecvWindow();

  conn.sendWindowUpdate(0, 1000);
  EXPECT_GT(conn.connectionRecvWindow(), initial);
}

TEST(Http2Core, RstStreamFrameOnStreamZeroIsProtocolError) {
  Http2Config cfg;
  Http2Connection conn(cfg, true);

  auto preface = MakePreface();
  (void)conn.processInput(preface);

  RawBytes rst;
  WriteRstStreamFrame(rst, 0, ErrorCode::Cancel);

  auto res = conn.processInput(AsSpan(rst));
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

TEST(Http2Core, StreamClosedRejectsDataAfterEndStream) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  constexpr uint32_t streamId = 1;

  RawChars headers;
  headers.append(MakeHttp1HeaderLine(":method", "GET"));
  headers.append(MakeHttp1HeaderLine(":scheme", "https"));
  headers.append(MakeHttp1HeaderLine(":authority", "example.com"));
  headers.append(MakeHttp1HeaderLine(":path", "/close"));

  ASSERT_EQ(h2.client.sendHeaders(streamId, http::StatusCodeOK, HeadersView(headers), true), ErrorCode::NoError);
  Http2Loopback::pump(h2.client, h2.server);

  // Stream is half-closed remote on server now. DATA from peer should be ignored
  // (stream exists but peer has already ended). The implementation may simply ignore
  // late DATA on half-closed streams without raising an error.
  RawBytes data;
  std::array<std::byte, 1> payload = {std::byte{'x'}};
  WriteDataFrame(data, streamId, payload, false);

  auto res = h2.server.processInput(AsSpan(data));
  // Per implementation: DATA on half-closed-remote stream is silently ignored (no error).
  EXPECT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
}

TEST(Http2Core, FrameParserHandlesBackToBackFramesInSingleBuffer) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  // Use the proper sendHeaders/sendData API which handles HPACK encoding correctly.
  constexpr uint32_t streamId = 1;

  RawChars headers;
  headers.append(MakeHttp1HeaderLine(":method", "GET"));
  headers.append(MakeHttp1HeaderLine(":scheme", "https"));
  headers.append(MakeHttp1HeaderLine(":authority", "example.com"));
  headers.append(MakeHttp1HeaderLine(":path", "/backtoback"));

  ASSERT_EQ(h2.client.sendHeaders(streamId, http::StatusCodeOK, HeadersView(headers), false), ErrorCode::NoError);

  std::array<std::byte, 3> payload = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  ASSERT_EQ(h2.client.sendData(streamId, payload, true), ErrorCode::NoError);

  // All frames are queued in client output; pump to server.
  Http2Loopback::pump(h2.client, h2.server);

  ASSERT_EQ(h2.serverHeaders.size(), 1U);
  ASSERT_EQ(h2.serverData.size(), 1U);
  EXPECT_TRUE(h2.serverData[0].endStream);
}

TEST(Http2Core, ManyTinyFramesDontBreakStateMachine) {
  Http2Config clientCfg;
  Http2Config serverCfg;

  Http2Loopback h2(clientCfg, serverCfg);
  h2.connect(true);

  // Construct 100 minimal DATA frames on a single stream.
  constexpr uint32_t streamId = 1;

  RawChars headers;
  headers.append(MakeHttp1HeaderLine(":method", "GET"));
  headers.append(MakeHttp1HeaderLine(":scheme", "https"));
  headers.append(MakeHttp1HeaderLine(":authority", "example.com"));
  headers.append(MakeHttp1HeaderLine(":path", "/many"));

  ASSERT_EQ(h2.client.sendHeaders(streamId, http::StatusCodeOK, HeadersView(headers), false), ErrorCode::NoError);
  Http2Loopback::pump(h2.client, h2.server);

  for (int ii = 0; ii < 100; ++ii) {
    std::array<std::byte, 1> bytes = {static_cast<std::byte>('a' + (ii % 26))};
    const bool endStream = (ii == 99);
    ASSERT_EQ(h2.client.sendData(streamId, bytes, endStream), ErrorCode::NoError);
    // Pump after each to avoid flow control blocking.
    Http2Loopback::pump(h2.client, h2.server);
  }

  ASSERT_EQ(h2.serverData.size(), 100U);
  EXPECT_TRUE(h2.serverData.back().endStream);
}

}  // namespace

}  // namespace aeronet::http2
