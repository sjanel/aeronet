#include "aeronet/http2-connection.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <utility>

#include "aeronet/hpack.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/http2-process-result-error-msg.hpp"
#include "aeronet/http2-stream.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/sv-to-sv-map.hpp"
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
  vector<std::byte> headerBlockBytes;
  vector<std::pair<std::string, std::string>> headers;
};

[[nodiscard]] WireDecodedHeadersDebug DecodeFirstHeadersFromOutput(std::span<const std::byte> output) {
  HpackDecoder decoder(4096, true);

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
      dbg.headers.reserve(static_cast<uint32_t>(decodeResult.decodedHeaders.size()));
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
  preface.reserve(static_cast<decltype(preface)::size_type>(kConnectionPreface.size()));
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
[[nodiscard]] vector<std::byte> DrainPendingOutput(Http2Connection& connection) {
  vector<std::byte> output;
  output.reserve(static_cast<decltype(output)::size_type>(connection.pendingOutputSize()));

  vector<std::string_view> fragments;
  while (connection.hasPendingOutput()) {
    connection.getPendingOutputFragments(fragments);
    if (fragments.empty()) {
      ADD_FAILURE() << "Pending output exposed no fragments";
      break;
    }

    std::size_t bytesWritten = 0;
    for (const std::string_view fragment : fragments) {
      const auto bytes = std::as_bytes(std::span<const char>(fragment.data(), fragment.size()));
      std::ranges::copy(bytes, std::back_inserter(output));
      bytesWritten += fragment.size();
    }
    connection.onOutputWritten(bytesWritten);
  }
  return output;
}

void FeedConnection(Http2Connection& connection, std::span<const std::byte> input) {
  while (!input.empty()) {
    const auto result = connection.processInput(input);
    ASSERT_NE(result.action, Http2Connection::ProcessResult::Action::Error);
    if (result.bytesConsumed == 0) {
      ADD_FAILURE() << "No progress feeding HTTP/2 connection";
      return;
    }
    input = input.subspan(result.bytesConsumed);
  }
}

void PumpOutput(Http2Connection& source, Http2Connection& destination) {
  const auto output = DrainPendingOutput(source);
  if (!output.empty()) {
    FeedConnection(destination, output);
  }
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

TEST(Http2Connection, ClientCanCreateStreamBeforeServerSettings) {
  // RFC 9113 §3.4: a client may send requests immediately after its connection preface, without waiting
  // for the server's SETTINGS (the peer settings hold the RFC defaults until they arrive).
  Http2Config config;
  Http2Connection client(config, false);

  EXPECT_FALSE(client.canCreateStreams());  // preface not sent yet
  client.sendClientPreface();
  EXPECT_TRUE(client.canCreateStreams());

  RawChars headers;
  headers.append(MakeHttp1HeaderLine(":method", "GET"));
  headers.append(MakeHttp1HeaderLine(":scheme", "http"));
  headers.append(MakeHttp1HeaderLine(":authority", "example.com"));
  headers.append(MakeHttp1HeaderLine(":path", "/"));
  EXPECT_EQ(client.sendHeaders(1, http::StatusCode{}, HeadersView(headers), true), ErrorCode::NoError);
}

TEST(Http2Connection, SequentialExchangesReleaseActiveStreamsOnBothSides) {
  // A full request/response exchange (client HEADERS+END_STREAM, server HEADERS + DATA+END_STREAM) must
  // drop the active-stream count back to zero on BOTH sides -- including the server, whose stream reaches
  // the Closed state through the *send* path (send END_STREAM while half-closed remote).
  Http2Config config;
  Http2Connection server(config, true);
  Http2Connection client(config, false);

  client.sendClientPreface();
  const auto pump = [&] {
    for (int iter = 0; iter < 8 && (client.hasPendingOutput() || server.hasPendingOutput()); ++iter) {
      if (client.hasPendingOutput()) {
        PumpOutput(client, server);
      }
      if (server.hasPendingOutput()) {
        PumpOutput(server, client);
      }
    }
  };
  pump();
  ASSERT_TRUE(client.isOpen());
  ASSERT_TRUE(server.isOpen());

  for (uint32_t streamId = 1; streamId <= 5; streamId += 2) {
    RawChars reqHeaders;
    reqHeaders.append(MakeHttp1HeaderLine(":method", "GET"));
    reqHeaders.append(MakeHttp1HeaderLine(":scheme", "http"));
    reqHeaders.append(MakeHttp1HeaderLine(":authority", "example.com"));
    reqHeaders.append(MakeHttp1HeaderLine(":path", "/"));
    ASSERT_EQ(client.sendHeaders(streamId, http::StatusCode{}, HeadersView(reqHeaders), true), ErrorCode::NoError);
    pump();

    ASSERT_EQ(server.sendHeaders(streamId, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);
    const std::array<std::byte, 4> body{std::byte{'b'}, std::byte{'o'}, std::byte{'d'}, std::byte{'y'}};
    ASSERT_EQ(server.sendData(streamId, body, true), ErrorCode::NoError);
    // The server stream reached Closed through the send path, outside frame processing: its lifecycle
    // must be finalized explicitly (the server's protocol handler does this for deferred completions).
    EXPECT_EQ(server.activeStreamCount(), 1U);
    server.finalizeSendClosedStream(streamId);
    pump();

    EXPECT_EQ(client.activeStreamCount(), 0U) << "client leaks streams after exchange on stream " << streamId;
    EXPECT_EQ(server.activeStreamCount(), 0U) << "server leaks streams after exchange on stream " << streamId;
  }
}

TEST(Http2Connection, ServerCannotCreateStreamBeforeSettings) {
  Http2Config config;
  Http2Connection server(config, true);
  // The client-side allowance must not leak into server mode: a server never creates streams before the
  // connection is fully established.
  EXPECT_FALSE(server.canCreateStreams());
}

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

TEST(Http2Connection, InputPausesAtOutputHighWaterMarkUntilSlowReaderDrains) {
  Http2Connection connection(Http2Config{}, true);
  AdvanceToOpenAndDrainSettingsAck(connection);

  RawBytes input;
  WritePingFrame(input, PingFrame{});
  WritePingFrame(input, PingFrame{});
  const auto bytes = std::span<const std::byte>(input.data(), input.size());
  static constexpr std::size_t kPingFrameSize = FrameHeader::kSize + 8U;

  const auto first = connection.processInput(bytes, kPingFrameSize);
  EXPECT_EQ(first.bytesConsumed, kPingFrameSize);
  EXPECT_EQ(connection.pendingOutputSize(), kPingFrameSize);

  const auto stalled = connection.processInput(bytes.subspan(first.bytesConsumed), kPingFrameSize);
  EXPECT_EQ(stalled.bytesConsumed, 0U);
  EXPECT_EQ(connection.pendingOutputSize(), kPingFrameSize);

  connection.onOutputWritten(kPingFrameSize);
  const auto resumed = connection.processInput(bytes.subspan(first.bytesConsumed), kPingFrameSize);
  EXPECT_EQ(resumed.bytesConsumed, kPingFrameSize);
  EXPECT_EQ(connection.pendingOutputSize(), kPingFrameSize);
}

TEST(Http2Connection, SmallResponsesShareOneOutputFragment) {
  Http2Config config;
  Http2Connection connection(config, true);
  AdvanceToOpenAndDrainSettingsAck(connection);

  static constexpr std::array<std::byte, 4> kBody{std::byte{'p'}, std::byte{'o'}, std::byte{'n'}, std::byte{'g'}};
  for (const uint32_t streamId : {1U, 3U}) {
    ASSERT_EQ(connection.sendHeaders(streamId, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);
    RawBytes owner(kBody.size());
    owner.unchecked_append(kBody);
    ASSERT_EQ(connection.sendData(streamId, std::move(owner), 0, kBody.size(), true), ErrorCode::NoError);
  }

  vector<std::string_view> fragments;
  connection.getPendingOutputFragments(fragments);
  ASSERT_EQ(fragments.size(), 1U);

  const auto wire = std::as_bytes(std::span(fragments.front()));
  std::size_t pos = 0;
  for (const uint32_t streamId : {1U, 3U}) {
    const FrameHeader headers = ParseFrameHeader(wire.subspan(pos));
    EXPECT_EQ(headers.type, FrameType::Headers);
    EXPECT_EQ(headers.streamId, streamId);
    pos += FrameHeader::kSize + headers.length;

    const FrameHeader data = ParseFrameHeader(wire.subspan(pos));
    EXPECT_EQ(data.type, FrameType::Data);
    EXPECT_EQ(data.streamId, streamId);
    EXPECT_EQ(data.flags, FrameFlags::DataEndStream);
    ASSERT_EQ(data.length, kBody.size());
    EXPECT_TRUE(std::ranges::equal(wire.subspan(pos + FrameHeader::kSize, data.length), kBody));
    pos += FrameHeader::kSize + data.length;
  }
  EXPECT_EQ(pos, wire.size());
}

TEST(Http2Connection, OwnedDataUsesGatherFragmentsAcrossPartialWrites) {
  Http2Config config;
  Http2Connection connection(config, true);
  AdvanceToOpenAndDrainSettingsAck(connection);

  ASSERT_EQ(connection.sendHeaders(1, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);
  (void)DrainPendingOutput(connection);

  constexpr std::size_t kPayloadSize = 20000;
  RawBytes owner(kPayloadSize);
  owner.setSize(kPayloadSize);
  std::ranges::fill(owner, std::byte{0x5A});
  const char* payloadData = reinterpret_cast<const char*>(owner.data());
  const std::size_t maxFrameSize = connection.peerSettings().maxFrameSize;

  ASSERT_EQ(connection.sendData(1, std::move(owner), 0, kPayloadSize, true), ErrorCode::NoError);
  EXPECT_EQ(connection.pendingOutputSize(), kPayloadSize + (2 * FrameHeader::kSize));

  vector<std::string_view> fragments;
  connection.getPendingOutputFragments(fragments);
  ASSERT_EQ(fragments.size(), 4U);
  EXPECT_EQ(fragments[1].data(), payloadData);
  EXPECT_EQ(fragments[1].size(), maxFrameSize);
  EXPECT_EQ(fragments[3].data(), payloadData + maxFrameSize);
  EXPECT_EQ(fragments[3].size(), kPayloadSize - maxFrameSize);

  const auto firstHeader = ParseFrameHeader(std::as_bytes(std::span(fragments[0])));
  const auto secondHeader = ParseFrameHeader(std::as_bytes(std::span(fragments[2])));
  EXPECT_EQ(firstHeader.type, FrameType::Data);
  EXPECT_EQ(firstHeader.length, maxFrameSize);
  EXPECT_EQ(firstHeader.flags, FrameFlags::None);
  EXPECT_EQ(secondHeader.type, FrameType::Data);
  EXPECT_EQ(secondHeader.flags, FrameFlags::DataEndStream);

  connection.onOutputWritten(5);
  connection.getPendingOutputFragments(fragments);
  ASSERT_EQ(fragments.size(), 4U);
  EXPECT_EQ(fragments[0].size(), FrameHeader::kSize - 5U);

  constexpr std::size_t kSecondFramePartialPayload = 17;
  connection.onOutputWritten((FrameHeader::kSize - 5U) + maxFrameSize + FrameHeader::kSize +
                             kSecondFramePartialPayload);
  connection.getPendingOutputFragments(fragments);
  ASSERT_EQ(fragments.size(), 1U);
  EXPECT_EQ(fragments[0].data(), payloadData + maxFrameSize + kSecondFramePartialPayload);
  EXPECT_EQ(fragments[0].size(), kPayloadSize - maxFrameSize - kSecondFramePartialPayload);

  connection.onOutputWritten(connection.pendingOutputSize());
  EXPECT_FALSE(connection.hasPendingOutput());
}

TEST(Http2Connection, GatherFragmentsExposeEntireOutputQueue) {
  Http2Config config;
  Http2Connection connection(config, true);
  AdvanceToOpenAndDrainSettingsAck(connection);

  ASSERT_EQ(connection.sendHeaders(1, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);
  (void)DrainPendingOutput(connection);

  constexpr std::size_t kBlockCount = 40;
  constexpr std::size_t kPayloadSize = 512;
  for (std::size_t blockIndex = 0; blockIndex < kBlockCount; ++blockIndex) {
    RawBytes owner(kPayloadSize);
    owner.setSize(kPayloadSize);
    owner[0] = static_cast<std::byte>(blockIndex);
    ASSERT_EQ(connection.sendData(1, std::move(owner), 0, kPayloadSize, false), ErrorCode::NoError);
  }

  vector<std::string_view> fragments;
  connection.getPendingOutputFragments(fragments);
  ASSERT_EQ(fragments.size(), kBlockCount * 2U);

  std::size_t pendingSize = 0;
  for (const std::string_view fragment : fragments) {
    pendingSize += fragment.size();
  }
  EXPECT_EQ(pendingSize, kBlockCount * (FrameHeader::kSize + kPayloadSize));

  connection.onOutputWritten(pendingSize);
  EXPECT_FALSE(connection.hasPendingOutput());
}

TEST(Http2Connection, OversizedHeaderBlockGathersContinuationWithoutRecopy) {
  Http2Config config;
  Http2Connection connection(config, true);
  AdvanceToOpenAndDrainSettingsAck(connection);

  const std::string largeValue(30000, 'Q');
  RawChars headers;
  headers.append(MakeHttp1HeaderLine("x-large", largeValue));

  ASSERT_EQ(connection.sendHeaders(1, http::StatusCodeOK, HeadersView(headers), true), ErrorCode::NoError);

  vector<std::string_view> fragments;
  connection.getPendingOutputFragments(fragments);
  ASSERT_GE(fragments.size(), 4U);

  const auto firstHeader = ParseFrameHeader(std::as_bytes(std::span(fragments[0])));
  const auto continuationHeader = ParseFrameHeader(std::as_bytes(std::span(fragments[2])));
  EXPECT_EQ(firstHeader.type, FrameType::Headers);
  EXPECT_EQ(firstHeader.length, connection.peerSettings().maxFrameSize);
  EXPECT_EQ(continuationHeader.type, FrameType::Continuation);
  EXPECT_EQ(continuationHeader.streamId, 1U);
  EXPECT_EQ(fragments[1].data() + fragments[1].size(), fragments[3].data());

  const auto wire = DrainPendingOutput(connection);
  const auto decoded = DecodeFirstHeadersFromOutput(wire);
  ASSERT_TRUE(decoded.foundHeaders);
  ASSERT_TRUE(decoded.decodeSuccess);
  EXPECT_TRUE(std::ranges::any_of(
      decoded.headers, [&](const auto& header) { return header.first == "x-large" && header.second == largeValue; }));
}

TEST(Http2Connection, OversizedHeaderBlockPreservesEarlierBatchedFrames) {
  Http2Config config;
  Http2Connection connection(config, true);
  AdvanceToOpenAndDrainSettingsAck(connection);

  ASSERT_EQ(connection.sendHeaders(1, http::StatusCodeOK, HeadersView{}, true), ErrorCode::NoError);

  const std::string largeValue(30000, 'Q');
  RawChars headers;
  headers.append(MakeHttp1HeaderLine("x-large", largeValue));
  ASSERT_EQ(connection.sendHeaders(3, http::StatusCodeOK, HeadersView(headers), true), ErrorCode::NoError);

  vector<std::string_view> fragments;
  connection.getPendingOutputFragments(fragments);
  ASSERT_GE(fragments.size(), 5U);

  const FrameHeader earlierHeaders = ParseFrameHeader(std::as_bytes(std::span(fragments[0])));
  const FrameHeader largeHeaders = ParseFrameHeader(std::as_bytes(std::span(fragments[1])));
  const FrameHeader continuation = ParseFrameHeader(std::as_bytes(std::span(fragments[3])));
  EXPECT_EQ(earlierHeaders.type, FrameType::Headers);
  EXPECT_EQ(earlierHeaders.streamId, 1U);
  EXPECT_EQ(largeHeaders.type, FrameType::Headers);
  EXPECT_EQ(largeHeaders.streamId, 3U);
  EXPECT_EQ(continuation.type, FrameType::Continuation);
  EXPECT_EQ(continuation.streamId, 3U);
  EXPECT_EQ(fragments[2].data() + fragments[2].size(), fragments[4].data());
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
  SvToSvMap decoded;
  bool gotHeaders = false;
  client.setOnHeadersDecoded([&](uint32_t /*streamId*/, const SvToSvMap& headers, bool /*endStream*/) {
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

// RFC 9113 §8.2.1: an HTTP/2 field value must not carry leading or trailing OWS. The HTTP/1.1 serializer
// legitimately emits such OWS -- most notably the compression codec pads Content-Length with trailing spaces
// (http-codec.cpp) so it can rewrite the value in place -- so encodeHeaders() must trim before HPACK-encoding.
// A strict peer (nghttp2 / curl) otherwise treats the padded field as malformed and RST_STREAMs the response.
TEST(Http2Connection, ResponseHeaderValuesAreTrimmedOfSurroundingOws) {
  Http2Config config;
  Http2Connection server(config, true);
  AdvanceToOpenAndDrainSettingsAck(server);
  // Drain the server SETTINGS so the only HEADERS in the next output is the one we send below.
  while (server.hasPendingOutput()) {
    server.onOutputWritten(server.getPendingOutput().size());
  }

  RawChars headers;
  headers.append(MakeHttp1HeaderLine(":status", "200"));
  headers.append(MakeHttp1HeaderLine("content-type", "application/json"));
  // Trailing OWS, exactly as the compression codec's padded Content-Length arrives here.
  headers.append(MakeHttp1HeaderLine("content-length", "5979   "));
  // Both-sides OWS to exercise the leading trim as well.
  headers.append(MakeHttp1HeaderLine("x-padded", "  value  "));

  ASSERT_EQ(server.sendHeaders(1, http::StatusCode{}, HeadersView(headers), true), ErrorCode::NoError);

  ASSERT_TRUE(server.hasPendingOutput());
  const auto out = server.getPendingOutput();
  const auto wire = DecodeFirstHeadersFromOutput(out);
  ASSERT_TRUE(wire.foundHeaders) << "No HEADERS frame found in server output";
  ASSERT_TRUE(wire.decodeSuccess) << "Failed to HPACK-decode server HEADERS";

  auto valueOf = [&](std::string_view name) -> std::string {
    for (const auto& [hname, hvalue] : wire.headers) {
      if (hname == name) {
        return hvalue;
      }
    }
    ADD_FAILURE() << "header '" << name << "' not found in wire-decoded HEADERS";
    return {};
  };
  EXPECT_EQ(valueOf("content-length"), "5979");
  EXPECT_EQ(valueOf("x-padded"), "value");
  EXPECT_EQ(valueOf("content-type"), "application/json");  // untouched: no surrounding OWS
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
  conn.setOnStreamClosed([&closedCount](uint32_t) { ++closedCount; });
  conn.setOnStreamReset([&resetCount](uint32_t, ErrorCode) { ++resetCount; });

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
  conn.setOnStreamClosed([&closedCount](uint32_t) { ++closedCount; });
  conn.setOnStreamReset([&resetCount](uint32_t, ErrorCode) { ++resetCount; });

  ASSERT_EQ(conn.sendHeaders(1, http::StatusCodeOK, HeadersView(std::string{}), false), ErrorCode::NoError);
  EXPECT_EQ(conn.activeStreamCount(), 1U);

  const uint32_t code = static_cast<uint32_t>(ErrorCode::Cancel);
  const std::array payload{
      std::byte{static_cast<uint8_t>((code >> 24U) & 0xFFU)},
      std::byte{static_cast<uint8_t>((code >> 16U) & 0xFFU)},
      std::byte{static_cast<uint8_t>((code >> 8U) & 0xFFU)},
      std::byte{static_cast<uint8_t>(code & 0xFFU)},
  };
  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
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
  const std::array payload{
      std::byte{static_cast<uint8_t>((code >> 24U) & 0xFFU)},
      std::byte{static_cast<uint8_t>((code >> 16U) & 0xFFU)},
      std::byte{static_cast<uint8_t>((code >> 8U) & 0xFFU)},
      std::byte{static_cast<uint8_t>(code & 0xFFU)},
  };
  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
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
  conn.initiateGoAway(ErrorCode::NoError, ErrorMsg::NoError);

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

  conn.setOnHeadersDecoded([&](uint32_t, const SvToSvMap&, bool) { headersCalled = true; });
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

  PingFrame pingFrame;
  pingFrame.isAck = false;
  static constexpr std::byte opaqueData[]{
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
      std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
  };

  std::memcpy(pingFrame.opaqueData, opaqueData, sizeof(opaqueData));

  conn.sendPing(pingFrame);

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
  // Les entrées SETTINGS font 6 octets chacune ; 5 octets est invalide.
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  std::array payload{std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x10}};

  FrameHeader header{};
  header.length = static_cast<uint32_t>(payload.size());
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
  std::array entry{
      std::byte{0x00}, std::byte{0x02},                                    // SETTINGS_ENABLE_PUSH
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x02},  // value=2
  };

  FrameHeader header;
  header.length = static_cast<uint32_t>(entry.size());
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
  std::array entry{
      std::byte{0x00}, std::byte{0x05},                                    // SETTINGS_MAX_FRAME_SIZE
      std::byte{0x00}, std::byte{0x00}, std::byte{0x3F}, std::byte{0xFF},  // 16383
  };

  FrameHeader header;
  header.length = static_cast<uint32_t>(entry.size());
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
  std::array entry{
      std::byte{0x00}, std::byte{0x04},  // SETTINGS_INITIAL_WINDOW_SIZE
      std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
  };

  FrameHeader header;
  header.length = static_cast<uint32_t>(entry.size());
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
  std::array entry{
      std::byte{0xFF}, std::byte{0xFF},  // unknown ID 0xFFFF
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
  };

  FrameHeader header;
  header.length = static_cast<uint32_t>(entry.size());
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

TEST(Http2Connection, SettingsFrameEmptyAckIsAcceptedAndNoResponseSent) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  FrameHeader header{};
  header.length = 0;
  header.type = FrameType::Settings;
  header.flags = FrameFlags::SettingsAck;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, std::span<const std::byte>{});
  auto res = conn.processInput(bytes);

  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Continue);
  EXPECT_EQ(res.errorCode, ErrorCode::NoError);
  EXPECT_FALSE(conn.hasPendingOutput());
}

TEST(Http2Connection, SettingsAckFrameWithPayloadIsFrameSizeError) {
  // Un ACK doit avoir un payload vide (RFC 9113 §6.5).
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  std::array payload{
      std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
  };

  FrameHeader header{};
  header.length = static_cast<uint32_t>(payload.size());
  header.type = FrameType::Settings;
  header.flags = FrameFlags::SettingsAck;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);

  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FrameSizeError);
}

TEST(Http2Connection, SettingsFrameWithEntriesAppliesSettingsAndSendsAck) {
  // Fusionne ParseSettingsFrameWithEntries + RoundTripSettingsFrame :
  // 3 entrées, on vérifie l'acceptation et l'ACK renvoyé.
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  std::array<std::byte, 18> payload = {
      std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x20}, std::byte{0x00},  // HEADER_TABLE_SIZE=8192
      std::byte{0x00}, std::byte{0x03}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x32},  // MAX_CONCURRENT_STREAMS=50
      std::byte{0x00}, std::byte{0x05}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x80}, std::byte{0x00},  // MAX_FRAME_SIZE=32768
  };

  FrameHeader header{};
  header.length = static_cast<uint32_t>(payload.size());
  header.type = FrameType::Settings;
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);

  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::OutputReady);
  EXPECT_EQ(res.errorCode, ErrorCode::NoError);

  ASSERT_TRUE(conn.hasPendingOutput());
  const auto out = conn.getPendingOutput();
  ASSERT_GE(out.size(), FrameHeader::kSize);
  FrameHeader ackHeader = ParseFrameHeader(out.subspan(0, FrameHeader::kSize));
  EXPECT_EQ(ackHeader.type, FrameType::Settings);
  EXPECT_TRUE(ackHeader.hasFlag(FrameFlags::SettingsAck));
  EXPECT_EQ(ackHeader.length, 0U);
}

TEST(Http2Connection, SettingsFrameInitialWindowSizeTooLargeIsFlowControlError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  std::array payload{std::byte{0x00}, std::byte{0x04}, std::byte{0x80},
                     std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};  // 0x80000000 > 0x7FFFFFFF

  FrameHeader header{};
  header.length = static_cast<uint32_t>(payload.size());
  header.type = FrameType::Settings;
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);

  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FlowControlError);
}

TEST(Http2Connection, PingFrameOnNonZeroStreamIsProtocolError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  constexpr std::array payload{
      std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8},
  };
  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
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

  constexpr std::array payload{
      std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7},
  };
  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
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

  std::array payload{
      std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD},
      std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
  };
  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
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

  const std::byte payload[]{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
  };
  FrameHeader header;
  header.length = static_cast<uint32_t>(sizeof(payload));
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
  std::array payload{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
  };
  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
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
  header.length = static_cast<uint32_t>(payload.size());
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
  std::array payload{
      std::byte{static_cast<uint8_t>((increment >> 24U) & 0xFFU)},
      std::byte{static_cast<uint8_t>((increment >> 16U) & 0xFFU)},
      std::byte{static_cast<uint8_t>((increment >> 8U) & 0xFFU)},
      std::byte{static_cast<uint8_t>(increment & 0xFFU)},
  };

  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
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
  vector<std::byte> payload(initialWindow, std::byte{0});

  EXPECT_EQ(conn.sendData(1U, payload, false), ErrorCode::NoError);

  // Now send SETTINGS_INITIAL_WINDOW_SIZE = 0 which should cause updateInitialWindowSize
  // to compute newWindow < 0 for the existing stream and return FlowControlError.
  std::array entry{
      std::byte{0x00}, std::byte{0x04},  // SETTINGS_INITIAL_WINDOW_SIZE
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
  };

  FrameHeader header;
  header.length = static_cast<uint32_t>(entry.size());
  header.type = FrameType::Settings;
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, entry);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FlowControlError);
  EXPECT_TRUE(conn.hasPendingOutput());
  const auto output = DrainPendingOutput(conn);
  const auto out = std::span<const std::byte>(output);
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
  Http2Stream* pStream = conn.getStream(1);
  ASSERT_NE(pStream, nullptr);

  const uint32_t increment =
      static_cast<uint32_t>(static_cast<int64_t>(kMaxWindow) - static_cast<int64_t>(initialWindow));
  const ErrorCode incErr = pStream->increaseSendWindow(increment);
  EXPECT_EQ(incErr, ErrorCode::NoError);

  // Now apply SETTINGS_INITIAL_WINDOW_SIZE = initialWindow + 1 which will
  // cause newWindow = kMaxWindow + 1 -> overflow and should return FlowControlError
  const uint32_t newInitial = initialWindow + 1U;
  std::array entry{
      std::byte{0x00},
      std::byte{0x04},  // SETTINGS_INITIAL_WINDOW_SIZE
      std::byte{static_cast<uint8_t>((newInitial >> 24U) & 0xFFU)},
      std::byte{static_cast<uint8_t>((newInitial >> 16U) & 0xFFU)},
      std::byte{static_cast<uint8_t>((newInitial >> 8U) & 0xFFU)},
      std::byte{static_cast<uint8_t>(newInitial & 0xFFU)},
  };

  FrameHeader header;
  header.length = static_cast<uint32_t>(entry.size());
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
  header.length = static_cast<uint32_t>(payload.size());
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
  Http2Stream* pStream = conn.getStream(1);
  ASSERT_NE(pStream, nullptr);

  const uint32_t increment =
      static_cast<uint32_t>(static_cast<int64_t>(kMaxWindow) - static_cast<int64_t>(initialWindow));
  const ErrorCode incErr = pStream->increaseSendWindow(increment);
  EXPECT_EQ(incErr, ErrorCode::NoError);

  // Now send a WINDOW_UPDATE for the stream with increment 1 which will overflow
  const uint32_t winInc = 1U;
  std::array payload{
      std::byte{static_cast<uint8_t>((winInc >> 24U) & 0xFFU)},
      std::byte{static_cast<uint8_t>((winInc >> 16U) & 0xFFU)},
      std::byte{static_cast<uint8_t>((winInc >> 8U) & 0xFFU)},
      std::byte{static_cast<uint8_t>(winInc & 0xFFU)},
  };

  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
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
  header.length = static_cast<uint32_t>(payload.size());
  header.type = FrameType::Continuation;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

namespace {

void WriteContinuationFrame(RawBytes& buffer, uint32_t streamId, std::span<const std::byte> headerBlock) {
  WriteFrame(buffer, FrameType::Continuation, FrameFlags::ContinuationEndHeaders, streamId,
             static_cast<uint32_t>(headerBlock.size()));
  buffer.unchecked_append(headerBlock);
}

}  // namespace

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
  WriteContinuationFrame(buffer, 1, hb_fragment);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  auto res = conn.processInput(span);

  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::InternalError);
  EXPECT_TRUE(conn.hasPendingOutput());

  // Pending output should contain GOAWAY
  const auto output = DrainPendingOutput(conn);
  const auto out = std::span<const std::byte>(output);
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
  header.length = static_cast<uint32_t>(payload.size());
  header.type = FrameType::PushPromise;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

// ============================
// Unknown frame type coverage
// ============================

TEST(Http2Connection, UnknownFrameTypeIsIgnored) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Frame type 0xFE is unknown and should be silently ignored per RFC 9113 §4.1
  std::array<std::byte, 4> payload = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
  header.type = static_cast<FrameType>(0xFE);
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_TRUE(conn.isOpen());
}

// ============================
// DATA frame error path coverage
// ============================

TEST(Http2Connection, DataFrameWithInvalidPaddingIsProtocolError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Create a stream first
  ASSERT_EQ(conn.sendHeaders(1, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Build a DATA frame with PADDED flag (0x08) where pad length exceeds payload.
  // Frame: [pad_length=0xFF] [data...] — pad_length of 255 but total payload is only 2 bytes.
  std::array<std::byte, 2> payload = {std::byte{0xFF}, std::byte{0x00}};
  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
  header.type = FrameType::Data;
  header.flags = FrameFlags::DataPadded;  // 0x08
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

TEST(Http2Connection, DataFrameExceedsConnectionRecvWindow) {
  Http2Config config;
  // Use a small connection window to make the test feasible.
  config.connectionWindowSize = 100;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Disable the onData callback to prevent automatic WINDOW_UPDATE
  conn.setOnData(nullptr);

  // Create a stream
  ASSERT_EQ(conn.sendHeaders(1, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Send a DATA frame larger than the connection recv window (100 bytes).
  // We need to construct a raw frame because the peer doesn't know our window.
  vector<std::byte> payload(200, std::byte{0x42});
  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
  header.type = FrameType::Data;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FlowControlError);
}

TEST(Http2Connection, DataWindowUpdatesAreBatchedAndSkipClosedStream) {
  Http2Config config;
  config.connectionWindowSize = 100000;
  config.maxFrameSize = 40000;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);
  conn.setOnData([](uint32_t, std::span<const std::byte>, bool) {});

  ASSERT_EQ(conn.sendHeaders(1, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);
  (void)DrainPendingOutput(conn);

  const auto feedData = [&conn](std::size_t size, bool endStream) {
    vector<std::byte> payload(static_cast<vector<std::byte>::size_type>(size), std::byte{0x42});
    const FrameHeader header{static_cast<uint32_t>(size), FrameType::Data,
                             endStream ? FrameFlags::DataEndStream : FrameFlags::None, 1};
    FeedConnection(conn, SerializeFrame(header, payload));
  };

  feedData(30000, false);
  EXPECT_FALSE(conn.hasPendingOutput());
  ASSERT_NE(conn.getStream(1), nullptr);
  EXPECT_EQ(conn.getStream(1)->recvWindow(), 35535);
  EXPECT_EQ(conn.connectionRecvWindow(), 70000);

  feedData(3000, false);
  auto output = DrainPendingOutput(conn);
  ASSERT_EQ(output.size(), FrameHeader::kSize + sizeof(uint32_t));
  auto header = ParseFrameHeader(output);
  EXPECT_EQ(header.type, FrameType::WindowUpdate);
  EXPECT_EQ(header.streamId, 1U);
  WindowUpdateFrame update{};
  ASSERT_EQ(ParseWindowUpdateFrame(std::span<const std::byte>(output).subspan(FrameHeader::kSize), update),
            FrameParseResult::Ok);
  EXPECT_EQ(update.windowSizeIncrement, 33000U);
  EXPECT_EQ(conn.getStream(1)->recvWindow(), 65535);
  EXPECT_EQ(conn.connectionRecvWindow(), 67000);

  feedData(17000, true);
  output = DrainPendingOutput(conn);
  ASSERT_EQ(output.size(), FrameHeader::kSize + sizeof(uint32_t));
  header = ParseFrameHeader(output);
  EXPECT_EQ(header.type, FrameType::WindowUpdate);
  EXPECT_EQ(header.streamId, 0U);
  ASSERT_EQ(ParseWindowUpdateFrame(std::span<const std::byte>(output).subspan(FrameHeader::kSize), update),
            FrameParseResult::Ok);
  EXPECT_EQ(update.windowSizeIncrement, 50000U);
  EXPECT_EQ(conn.connectionRecvWindow(), 100000);
}

TEST(Http2Connection, PeerInitialWindowOnlyControlsNewStreamSendWindow) {
  Http2Config config;
  config.connectionWindowSize = 1U << 20U;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);
  conn.setOnData([](uint32_t, std::span<const std::byte>, bool) {});

  static constexpr uint32_t kPeerInitialWindow = 1U << 30U;
  RawBytes settings;
  const std::array settingsEntries{
      SettingsEntry{SettingsParameter::InitialWindowSize, kPeerInitialWindow},
  };
  WriteSettingsFrame(settings, settingsEntries);
  FeedConnection(conn, std::span<const std::byte>(settings.data(), settings.size()));
  (void)DrainPendingOutput(conn);
  ASSERT_EQ(conn.state(), ConnectionState::Open);

  HpackEncoder encoder(config.headerTableSize);
  RawBytes headerBlock;
  encoder.encode(headerBlock, ":method", "POST");
  encoder.encode(headerBlock, ":scheme", "https");
  encoder.encode(headerBlock, ":authority", "example.com");
  encoder.encode(headerBlock, ":path", "/upload");

  RawBytes headers;
  WriteFrame(headers, FrameType::Headers, ComputeHeaderFrameFlags(false, true), 1,
             static_cast<uint32_t>(headerBlock.size()));
  headers.unchecked_append(std::span<const std::byte>(headerBlock.data(), headerBlock.size()));
  FeedConnection(conn, std::span<const std::byte>(headers.data(), headers.size()));

  const Http2Stream* stream = conn.getStream(1);
  ASSERT_NE(stream, nullptr);
  EXPECT_EQ(stream->sendWindow(), static_cast<int32_t>(kPeerInitialWindow));
  EXPECT_EQ(stream->recvWindow(), static_cast<int32_t>(config.initialWindowSize));

  const std::array<std::byte, 16384> data{};
  const FrameHeader dataHeader{static_cast<uint32_t>(data.size()), FrameType::Data, FrameFlags::None, 1};
  FeedConnection(conn, SerializeFrame(dataHeader, data));
  EXPECT_FALSE(conn.hasPendingOutput());

  FeedConnection(conn, SerializeFrame(dataHeader, data));
  const auto output = DrainPendingOutput(conn);
  ASSERT_EQ(output.size(), FrameHeader::kSize + sizeof(uint32_t));
  const auto outputHeader = ParseFrameHeader(output);
  EXPECT_EQ(outputHeader.type, FrameType::WindowUpdate);
  EXPECT_EQ(outputHeader.streamId, 1U);
}

TEST(Http2Connection, DataFrameOnResetStreamIsIgnored) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Create stream and close it via RST_STREAM
  ASSERT_EQ(conn.sendHeaders(1, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);
  conn.sendRstStream(1, ErrorCode::Cancel);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Close enough streams to prune stream 1 from the map
  for (uint32_t idx = 0; idx < kClosedStreamsMaxRetainedForTest + 2U; ++idx) {
    const uint32_t sid = 3U + (idx * 2U);
    ASSERT_EQ(conn.sendHeaders(sid, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);
    conn.sendRstStream(sid, ErrorCode::Cancel);
  }
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Now send a DATA frame for pruned stream 1 — should be silently ignored
  std::array payload{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
  header.type = FrameType::Data;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
}

TEST(Http2Connection, DataFrameExceedsStreamRecvWindow) {
  Http2Config config;
  config.connectionWindowSize = 1U << 20U;  // Large connection window (1MB)
  config.maxFrameSize = 100000;             // Allow frames up to 100KB
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Create a stream (default stream recv window = _peerSettings.initialWindowSize = 65535)
  ASSERT_EQ(conn.sendHeaders(1, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Send a DATA frame exceeding the stream recv window (65535 bytes)
  vector<std::byte> payload(66000, std::byte{0x42});
  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
  header.type = FrameType::Data;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  // Stream error produces RST_STREAM in output, but processFrames returns NoError
  EXPECT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
  ASSERT_TRUE(conn.hasPendingOutput());
  const auto out = conn.getPendingOutput();
  ASSERT_GE(out.size(), FrameHeader::kSize);
  const auto outHeader = ParseFrameHeader(out);
  EXPECT_EQ(outHeader.type, FrameType::RstStream);
}

TEST(Http2Connection, DataFrameOnHalfClosedRemoteIsStreamError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Have client send HEADERS with END_STREAM for stream 1 → HalfClosedRemote on server
  HpackEncoder encoder(4096);
  RawBytes headerBlock;
  encoder.encode(headerBlock, ":method", "GET");
  encoder.encode(headerBlock, ":path", "/");
  encoder.encode(headerBlock, ":scheme", "https");
  encoder.encode(headerBlock, ":authority", "localhost");

  RawBytes headersBuf;
  WriteFrame(headersBuf, FrameType::Headers, ComputeHeaderFrameFlags(true, true), 1,
             static_cast<uint32_t>(headerBlock.size()));
  headersBuf.unchecked_append(
      std::span<const std::byte>(reinterpret_cast<const std::byte*>(headerBlock.data()), headerBlock.size()));

  auto hdrSpan = std::span<const std::byte>(reinterpret_cast<const std::byte*>(headersBuf.data()), headersBuf.size());
  auto resHdr = conn.processInput(hdrSpan);
  ASSERT_NE(resHdr.action, Http2Connection::ProcessResult::Action::Error);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Now send DATA on stream 1 → canReceive() false (HalfClosedRemote) → stream error
  std::array<std::byte, 4> payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
  header.type = FrameType::Data;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
  ASSERT_TRUE(conn.hasPendingOutput());
  const auto out = conn.getPendingOutput();
  ASSERT_GE(out.size(), FrameHeader::kSize);
  const auto outHeader = ParseFrameHeader(out);
  EXPECT_EQ(outHeader.type, FrameType::RstStream);
}

// ============================
// HEADERS frame error path coverage
// ============================

TEST(Http2Connection, HeadersFrameWithInvalidPaddingIsProtocolError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Build a HEADERS frame with PADDED flag where pad length exceeds payload.
  // flags: PADDED (0x08) | END_HEADERS (0x04)
  std::array<std::byte, 2> payload = {std::byte{0xFF}, std::byte{0x82}};  // pad_length=255, minimal HPACK
  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
  header.type = FrameType::Headers;
  header.flags = FrameFlags::HeadersEndHeaders | FrameFlags::HeadersPadded;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

TEST(Http2Connection, HeadersEvenStreamIdFromClientIsProtocolError) {
  Http2Config config;
  Http2Connection conn(config, true);  // server
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Send a HEADERS frame with even stream ID (server-initiated) from a client peer
  HpackEncoder encoder(4096);
  RawBytes headerBlock;
  encoder.encode(headerBlock, ":method", "GET");
  encoder.encode(headerBlock, ":path", "/");
  encoder.encode(headerBlock, ":scheme", "https");
  encoder.encode(headerBlock, ":authority", "localhost");

  RawBytes buf;
  WriteFrame(buf, FrameType::Headers, ComputeHeaderFrameFlags(true, true), 2,
             static_cast<uint32_t>(headerBlock.size()));
  buf.unchecked_append(
      std::span<const std::byte>(reinterpret_cast<const std::byte*>(headerBlock.data()), headerBlock.size()));

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size());
  auto res = conn.processInput(span);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

TEST(Http2Connection, HeadersStreamIdNotIncreasingIsProtocolError) {
  Http2Config config;
  Http2Connection conn(config, true);  // server
  AdvanceToOpenAndDrainSettingsAck(conn);

  HpackEncoder encoder(4096);

  // Send HEADERS for stream 3 first
  {
    RawBytes headerBlock;
    encoder.encode(headerBlock, ":method", "GET");
    encoder.encode(headerBlock, ":path", "/first");
    encoder.encode(headerBlock, ":scheme", "https");
    encoder.encode(headerBlock, ":authority", "localhost");

    RawBytes buf;
    WriteFrame(buf, FrameType::Headers, ComputeHeaderFrameFlags(true, true), 3,
               static_cast<uint32_t>(headerBlock.size()));
    buf.unchecked_append(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(headerBlock.data()), headerBlock.size()));

    auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size());
    auto res = conn.processInput(span);
    ASSERT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
  }
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Now send HEADERS for stream 1 (not increasing)
  {
    RawBytes headerBlock;
    encoder.encode(headerBlock, ":method", "GET");
    encoder.encode(headerBlock, ":path", "/second");
    encoder.encode(headerBlock, ":scheme", "https");
    encoder.encode(headerBlock, ":authority", "localhost");

    RawBytes buf;
    WriteFrame(buf, FrameType::Headers, ComputeHeaderFrameFlags(true, true), 1,
               static_cast<uint32_t>(headerBlock.size()));
    buf.unchecked_append(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(headerBlock.data()), headerBlock.size()));

    auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size());
    auto res = conn.processInput(span);
    EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
    EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
  }
}

TEST(Http2Connection, HeadersMaxConcurrentStreamsExceededIsProtocolError) {
  Http2Config config;
  config.maxConcurrentStreams = 1;
  Http2Connection conn(config, true);  // server
  AdvanceToOpenAndDrainSettingsAck(conn);

  HpackEncoder encoder(4096);

  // Send HEADERS for stream 1 (no END_STREAM to keep stream active)
  {
    RawBytes headerBlock;
    encoder.encode(headerBlock, ":method", "GET");
    encoder.encode(headerBlock, ":path", "/first");
    encoder.encode(headerBlock, ":scheme", "https");
    encoder.encode(headerBlock, ":authority", "localhost");

    RawBytes buf;
    WriteFrame(buf, FrameType::Headers, ComputeHeaderFrameFlags(false, true), 1,
               static_cast<uint32_t>(headerBlock.size()));
    buf.unchecked_append(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(headerBlock.data()), headerBlock.size()));

    auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size());
    auto res = conn.processInput(span);
    ASSERT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
  }
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Stream 1 is now open. Send HEADERS for stream 3 — should exceed max concurrent.
  {
    RawBytes headerBlock;
    encoder.encode(headerBlock, ":method", "GET");
    encoder.encode(headerBlock, ":path", "/second");
    encoder.encode(headerBlock, ":scheme", "https");
    encoder.encode(headerBlock, ":authority", "localhost");

    RawBytes buf;
    WriteFrame(buf, FrameType::Headers, ComputeHeaderFrameFlags(false, true), 3,
               static_cast<uint32_t>(headerBlock.size()));
    buf.unchecked_append(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(headerBlock.data()), headerBlock.size()));

    auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size());
    auto res = conn.processInput(span);
    EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
    EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
  }
}

TEST(Http2Connection, HeadersStreamDependsOnItselfIsStreamError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Build a HEADERS frame with PRIORITY flag where streamDependency == streamId
  RawBytes buf;
  std::array<std::byte, 1> hb = {std::byte{0x82}};
  // Stream 1 depends on stream 1 (itself)
  WriteHeadersFrameWithPriority(buf, 1, hb, 1, 16, false, false, true);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size());
  auto res = conn.processInput(span);
  // Stream error produces RST_STREAM in output
  EXPECT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
  ASSERT_TRUE(conn.hasPendingOutput());
  const auto out = conn.getPendingOutput();
  ASSERT_GE(out.size(), FrameHeader::kSize);
  const auto outHeader = ParseFrameHeader(out);
  EXPECT_EQ(outHeader.type, FrameType::RstStream);
}

TEST(Http2Connection, HeadersBlockTooLargeIsEnhanceYourCalm) {
  Http2Config config;
  config.maxFrameSize = 300000;  // Allow large frames to pass frame-size check
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Build a HEADERS frame without END_HEADERS so the header block is accumulated.
  // The header block fragment is larger than kMaxHeaderBlockAccumulationSize (256KB).
  vector<std::byte> hugeBlock(257 * 1024, std::byte{0x00});

  RawBytes buf;
  WriteFrame(buf, FrameType::Headers, ComputeHeaderFrameFlags(false, false), 1,
             static_cast<uint32_t>(hugeBlock.size()));
  buf.unchecked_append(std::span<const std::byte>(hugeBlock.data(), hugeBlock.size()));

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size());
  auto res = conn.processInput(span);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::EnhanceYourCalm);
}

TEST(Http2Connection, DecodedHeaderListOverLocalLimitResetsStream) {
  Http2Config config;
  config.maxHeaderListSize = 200;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  bool headersCalled = false;
  conn.setOnHeadersDecoded([&](uint32_t, const SvToSvMap&, bool) { headersCalled = true; });

  HpackEncoder encoder(config.headerTableSize);
  RawBytes headerBlock;
  encoder.encode(headerBlock, ":method", "GET");
  encoder.encode(headerBlock, ":scheme", "https");
  encoder.encode(headerBlock, ":authority", "example.com");
  encoder.encode(headerBlock, ":path", "/");
  encoder.encode(headerBlock, "x-large", "012345678901234567890123456789");

  RawBytes frame;
  WriteFrame(frame, FrameType::Headers, ComputeHeaderFrameFlags(true, true), 1,
             static_cast<uint32_t>(headerBlock.size()));
  frame.unchecked_append(std::span<const std::byte>(headerBlock.data(), headerBlock.size()));

  const auto input = std::span<const std::byte>(frame.data(), frame.size());
  const auto res = conn.processInput(input);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::OutputReady);
  EXPECT_FALSE(headersCalled);
  EXPECT_EQ(conn.state(), ConnectionState::Open);

  ASSERT_TRUE(conn.hasPendingOutput());
  EXPECT_EQ(ParseFrameHeader(conn.getPendingOutput()).type, FrameType::RstStream);
}

TEST(Http2Connection, HeadersHpackDecodingFailedIsCompressionError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Create invalid HPACK data — a dynamic table reference with index beyond what exists
  // Index 255 doesn't exist in the empty dynamic table
  std::array<std::byte, 2> invalidHpack = {std::byte{0x7F}, std::byte{0x80}};

  RawBytes buf;
  WriteFrame(buf, FrameType::Headers, ComputeHeaderFrameFlags(false, true), 1,
             static_cast<uint32_t>(invalidHpack.size()));
  buf.unchecked_append(std::span<const std::byte>(invalidHpack.data(), invalidHpack.size()));

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size());
  auto res = conn.processInput(span);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::CompressionError);
}

TEST(Http2Connection, MalformedFieldSectionResetsStreamWithoutClosingConnection) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  bool headersCalled = false;
  conn.setOnHeadersDecoded([&](uint32_t, const SvToSvMap&, bool) { headersCalled = true; });

  HpackEncoder encoder(config.headerTableSize);
  RawBytes headerBlock;
  encoder.encode(headerBlock, ":method", "GET");
  encoder.encode(headerBlock, ":method", "GET");

  RawBytes frame;
  WriteFrame(frame, FrameType::Headers, ComputeHeaderFrameFlags(true, true), 1,
             static_cast<uint32_t>(headerBlock.size()));
  frame.unchecked_append(headerBlock);

  const auto res = conn.processInput(frame);

  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::OutputReady);
  EXPECT_EQ(conn.state(), ConnectionState::Open);
  EXPECT_FALSE(headersCalled);
  ASSERT_TRUE(conn.hasPendingOutput());

  const auto output = conn.getPendingOutput();
  const auto outputHeader = ParseFrameHeader(output);
  EXPECT_EQ(outputHeader.type, FrameType::RstStream);
  RstStreamFrame reset;
  ASSERT_EQ(ParseRstStreamFrame(outputHeader, output.subspan(FrameHeader::kSize), reset), FrameParseResult::Ok);
  EXPECT_EQ(reset.errorCode, ErrorCode::ProtocolError);
}

TEST(Http2Connection, MalformedContinuationFieldSectionResetsStreamWithoutClosingConnection) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  bool headersCalled = false;
  conn.setOnHeadersDecoded([&](uint32_t, const SvToSvMap&, bool) { headersCalled = true; });

  HpackEncoder encoder(config.headerTableSize);
  RawBytes headerBlock;
  encoder.encode(headerBlock, ":method", "GET");
  encoder.encode(headerBlock, ":method", "GET");
  ASSERT_GT(headerBlock.size(), 1U);

  RawBytes headersFrame;
  WriteFrame(headersFrame, FrameType::Headers, ComputeHeaderFrameFlags(true, false), 1, 1U);
  headersFrame.unchecked_append(std::span<const std::byte>(headerBlock.data(), 1U));
  EXPECT_EQ(conn.processInput(headersFrame).action, Http2Connection::ProcessResult::Action::Continue);

  RawBytes continuationFrame;
  WriteContinuationFrame(continuationFrame, 1,
                         std::span<const std::byte>(headerBlock.data() + 1U, headerBlock.size() - 1U));
  const auto res = conn.processInput(continuationFrame);

  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::OutputReady);
  EXPECT_EQ(conn.state(), ConnectionState::Open);
  EXPECT_FALSE(headersCalled);
  ASSERT_TRUE(conn.hasPendingOutput());

  const auto output = conn.getPendingOutput();
  const auto outputHeader = ParseFrameHeader(output);
  EXPECT_EQ(outputHeader.type, FrameType::RstStream);
  RstStreamFrame reset;
  ASSERT_EQ(ParseRstStreamFrame(outputHeader, output.subspan(FrameHeader::kSize), reset), FrameParseResult::Ok);
  EXPECT_EQ(reset.errorCode, ErrorCode::ProtocolError);
}

// ============================
// PRIORITY frame error path coverage
// ============================

TEST(Http2Connection, PriorityFrameOnStreamZeroIsProtocolError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  RawBytes buf;
  WritePriorityFrame(buf, 0, 1, 16, false);

  // Rewrite stream ID to 0 in the serialized frame
  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size());
  auto res = conn.processInput(span);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

TEST(Http2Connection, PriorityFrameInvalidSizeIsFrameSizeError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // PRIORITY frame must be exactly 5 bytes. Send 3 bytes.
  std::array<std::byte, 3> payload = {std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};
  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
  header.type = FrameType::Priority;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FrameSizeError);
}

TEST(Http2Connection, PriorityFrameSelfDependencyIsStreamError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Create a stream first
  ASSERT_EQ(conn.sendHeaders(1, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Send PRIORITY where stream depends on itself
  RawBytes buf;
  WritePriorityFrame(buf, 1, 1, 16, false);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size());
  auto res = conn.processInput(span);
  // Stream error produces RST_STREAM in output
  EXPECT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
  ASSERT_TRUE(conn.hasPendingOutput());
  const auto out = conn.getPendingOutput();
  ASSERT_GE(out.size(), FrameHeader::kSize);
  const auto outHeader = ParseFrameHeader(out);
  EXPECT_EQ(outHeader.type, FrameType::RstStream);
}

TEST(Http2Connection, PriorityFrameFloodOnIdleStreamsIsEnhanceYourCalm) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Send > 10000 PRIORITY frames on non-existent (idle) streams
  for (uint32_t idx = 0; idx <= 10000; ++idx) {
    if (conn.hasPendingOutput()) {
      conn.onOutputWritten(conn.getPendingOutput().size());
    }
    const uint32_t fakeStreamId = 1U + (idx * 2U);
    RawBytes buf;
    WritePriorityFrame(buf, fakeStreamId, 0, 16, false);

    auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size());
    auto res = conn.processInput(span);
    if (idx < 10000) {
      ASSERT_NE(res.action, Http2Connection::ProcessResult::Action::Error) << "Failed at idx=" << idx;
    } else {
      EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
      EXPECT_EQ(res.errorCode, ErrorCode::EnhanceYourCalm);
    }
  }
}

// ============================
// RST_STREAM frame error path coverage
// ============================

TEST(Http2Connection, RstStreamFrameInvalidSizeIsFrameSizeError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // RST_STREAM payload must be exactly 4 bytes. Send 3 bytes.
  std::array<std::byte, 3> payload = {std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};
  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
  header.type = FrameType::RstStream;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FrameSizeError);
}

// ============================
// SETTINGS MAX_FRAME_SIZE upper bound coverage
// ============================

TEST(Http2Connection, SettingsMaxFrameSizeTooLargeIsProtocolError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToAwaitingSettingsAndDrainSettings(conn);

  // MAX_FRAME_SIZE must be <= 16777215. Provide 16777216 (0x01000000).
  std::array entry = {
      std::byte{0x00}, std::byte{0x05},                                    // SETTINGS_MAX_FRAME_SIZE
      std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},  // 16777216
  };

  FrameHeader header;
  header.length = static_cast<uint32_t>(entry.size());
  header.type = FrameType::Settings;
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, entry);
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::ProtocolError);
}

// ============================
// CONTINUATION frame error path coverage
// ============================

TEST(Http2Connection, ContinuationHeaderBlockTooLargeIsEnhanceYourCalm) {
  Http2Config config;
  config.maxFrameSize = 300000;  // Allow large frames to pass frame-size check
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Send HEADERS without END_HEADERS with a small fragment
  std::array<std::byte, 1> smallFragment = {std::byte{0x82}};
  RawBytes headersBuf;
  WriteFrame(headersBuf, FrameType::Headers, ComputeHeaderFrameFlags(false, false), 1,
             static_cast<uint32_t>(smallFragment.size()));
  headersBuf.unchecked_append(smallFragment);

  auto hdrSpan = std::span<const std::byte>(reinterpret_cast<const std::byte*>(headersBuf.data()), headersBuf.size());
  auto resHdr = conn.processInput(hdrSpan);
  ASSERT_NE(resHdr.action, Http2Connection::ProcessResult::Action::Error);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Send a CONTINUATION with a huge fragment that exceeds kMaxHeaderBlockAccumulationSize
  vector<std::byte> hugeBlock(257 * 1024, std::byte{0x00});
  RawBytes contBuf;
  WriteContinuationFrame(contBuf, 1, std::span<const std::byte>(hugeBlock.data(), hugeBlock.size()));

  auto contSpan = std::span<const std::byte>(reinterpret_cast<const std::byte*>(contBuf.data()), contBuf.size());
  auto res = conn.processInput(contSpan);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::EnhanceYourCalm);
}

TEST(Http2Connection, ContinuationDecodedHeaderListOverLocalLimitResetsStream) {
  Http2Config config;
  config.maxHeaderListSize = 200;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  bool headersCalled = false;
  conn.setOnHeadersDecoded([&](uint32_t, const SvToSvMap&, bool) { headersCalled = true; });

  HpackEncoder encoder(config.headerTableSize);
  RawBytes headerBlock;
  encoder.encode(headerBlock, ":method", "GET");
  encoder.encode(headerBlock, ":scheme", "https");
  encoder.encode(headerBlock, ":authority", "example.com");
  encoder.encode(headerBlock, ":path", "/");
  encoder.encode(headerBlock, "x-large", "012345678901234567890123456789");

  const std::size_t splitPos = headerBlock.size() / 2U;
  ASSERT_NE(splitPos, 0U);
  ASSERT_LT(splitPos, headerBlock.size());

  RawBytes headersFrame;
  WriteFrame(headersFrame, FrameType::Headers, ComputeHeaderFrameFlags(false, false), 1,
             static_cast<uint32_t>(splitPos));
  headersFrame.unchecked_append(std::span<const std::byte>(headerBlock.data(), splitPos));
  const auto headersInput = std::span<const std::byte>(headersFrame.data(), headersFrame.size());
  EXPECT_EQ(conn.processInput(headersInput).action, Http2Connection::ProcessResult::Action::Continue);

  RawBytes continuationFrame;
  WriteContinuationFrame(continuationFrame, 1,
                         std::span<const std::byte>(headerBlock.data() + splitPos, headerBlock.size() - splitPos));
  const auto continuationInput = std::span<const std::byte>(continuationFrame.data(), continuationFrame.size());
  EXPECT_EQ(conn.processInput(continuationInput).action, Http2Connection::ProcessResult::Action::OutputReady);
  EXPECT_FALSE(headersCalled);
  EXPECT_EQ(conn.state(), ConnectionState::Open);

  ASSERT_TRUE(conn.hasPendingOutput());
  const auto output = conn.getPendingOutput();
  const auto outputHeader = ParseFrameHeader(output);
  ASSERT_EQ(outputHeader.type, FrameType::RstStream);
  RstStreamFrame reset;
  const auto payload = output.subspan(FrameHeader::kSize, outputHeader.length);
  ASSERT_EQ(ParseRstStreamFrame(outputHeader, payload, reset), FrameParseResult::Ok);
  EXPECT_EQ(reset.errorCode, ErrorCode::EnhanceYourCalm);
}

TEST(Http2Connection, ContinuationHpackDecodeFailedIsCompressionError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Send HEADERS without END_HEADERS with a small valid-looking fragment
  std::array<std::byte, 1> fragment = {std::byte{0x82}};
  RawBytes headersBuf;
  WriteFrame(headersBuf, FrameType::Headers, ComputeHeaderFrameFlags(false, false), 1,
             static_cast<uint32_t>(fragment.size()));
  headersBuf.unchecked_append(fragment);

  auto hdrSpan = std::span<const std::byte>(reinterpret_cast<const std::byte*>(headersBuf.data()), headersBuf.size());
  auto resHdr = conn.processInput(hdrSpan);
  ASSERT_NE(resHdr.action, Http2Connection::ProcessResult::Action::Error);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Send CONTINUATION with END_HEADERS but invalid HPACK data.
  // The accumulated block will be [0x82] + invalid bytes.
  // 0x82 is ":method GET" indexed field. Appending invalid HPACK after it.
  std::array<std::byte, 2> invalidHpack = {std::byte{0x7F}, std::byte{0x80}};
  RawBytes contBuf;
  WriteContinuationFrame(contBuf, 1, invalidHpack);

  auto contSpan = std::span<const std::byte>(reinterpret_cast<const std::byte*>(contBuf.data()), contBuf.size());
  auto res = conn.processInput(contSpan);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::CompressionError);
}

// ============================
// sendData error path coverage
// ============================

TEST(Http2Connection, SendDataConnectionWindowRestoresStreamWindowOnOverflow) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Create a stream
  ASSERT_EQ(conn.sendHeaders(1, http::StatusCodeOK, HeadersView{}, false), ErrorCode::NoError);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Deplete connection send window (default 65535)
  vector<std::byte> big(static_cast<vector<std::byte>::size_type>(conn.connectionSendWindow()), std::byte{0x00});
  ASSERT_EQ(conn.sendData(1, big, false), ErrorCode::NoError);
  EXPECT_EQ(conn.connectionSendWindow(), 0);

  // Increase stream window to allow more data on the stream side
  Http2Stream* pStream = conn.getStream(1);
  ASSERT_NE(pStream, nullptr);
  (void)pStream->increaseSendWindow(1000);

  // Now try to send more data — stream has window but connection doesn't.
  // This should restore the stream window and return FlowControlError.
  std::array<std::byte, 1> extra = {std::byte{0x01}};
  EXPECT_EQ(conn.sendData(1, extra, false), ErrorCode::FlowControlError);

  // The stream window should have been restored
  EXPECT_GT(pStream->sendWindow(), 0);
}

// ============================
// GoAwaySent state processInput coverage
// ============================

TEST(Http2Connection, ProcessInputInGoAwaySentState) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Initiate GOAWAY
  conn.initiateGoAway(ErrorCode::NoError, ErrorMsg::NoError);
  ASSERT_EQ(conn.state(), ConnectionState::GoAwaySent);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Send a valid frame while in GoAwaySent state — should still process frames
  std::array pingPayload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                         std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
  FrameHeader header;
  header.length = static_cast<uint32_t>(pingPayload.size());
  header.type = FrameType::Ping;
  header.flags = FrameFlags::None;
  header.streamId = 0;

  auto bytes = SerializeFrame(header, pingPayload);
  auto res = conn.processInput(bytes);
  EXPECT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
  // Should still have produced a PING ACK response
  EXPECT_TRUE(conn.hasPendingOutput());
}

// ============================
// sendServerPreface / sendClientPreface edge cases
// ============================

TEST(Http2Connection, SendServerPrefaceDoesNothingIfAlreadySent) {
  Http2Config config;
  Http2Connection conn(config, true);

  // First call sends SETTINGS
  conn.sendServerPreface();
  EXPECT_TRUE(conn.hasPendingOutput());
  auto firstSize = conn.getPendingOutput().size();

  // Second call should be ignored
  conn.sendServerPreface();
  EXPECT_EQ(conn.getPendingOutput().size(), firstSize);
}

TEST(Http2Connection, SendServerPrefaceDoesNothingForClient) {
  Http2Config config;
  Http2Connection conn(config, false);  // client

  conn.sendServerPreface();
  EXPECT_FALSE(conn.hasPendingOutput());
}

TEST(Http2Connection, SendClientPrefaceDoesNothingIfAlreadySent) {
  Http2Config config;
  Http2Connection conn(config, false);  // client

  conn.sendClientPreface();
  EXPECT_TRUE(conn.hasPendingOutput());
  auto firstSize = conn.getPendingOutput().size();

  // Second call should be ignored
  conn.sendClientPreface();
  EXPECT_EQ(conn.getPendingOutput().size(), firstSize);
}

TEST(Http2Connection, SendClientPrefaceDoesNothingForServer) {
  Http2Config config;
  Http2Connection conn(config, true);  // server

  conn.sendClientPreface();
  EXPECT_FALSE(conn.hasPendingOutput());
}

// ============================
// Closed state coverage
// ============================

TEST(Http2Connection, ProcessInputInClosedStateReturnsClosed) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Force connection to closed state via a connection error
  std::array<std::byte, 4> payload = {std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};
  FrameHeader header;
  header.length = static_cast<uint32_t>(payload.size());
  header.type = FrameType::PushPromise;
  header.flags = FrameFlags::None;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, payload);
  (void)conn.processInput(bytes);
  ASSERT_EQ(conn.state(), ConnectionState::Closed);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Now processInput should return Closed
  std::array<std::byte, 1> moreBuf = {std::byte{0x00}};
  auto res = conn.processInput(moreBuf);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Closed);
}

// ============================
// sendHeaders error path coverage
// ============================

TEST(Http2Connection, SendHeadersOnHalfClosedLocalReturnsStreamClosed) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Send HEADERS with END_STREAM → stream 1 goes Idle → HalfClosedLocal
  ASSERT_EQ(conn.sendHeaders(1, http::StatusCodeOK, HeadersView{}, true), ErrorCode::NoError);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Try again on the same stream → onSendHeaders on HalfClosedLocal returns StreamClosed
  EXPECT_EQ(conn.sendHeaders(1, http::StatusCodeOK, HeadersView{}, false), ErrorCode::StreamClosed);
}

// ============================
// HEADERS invalid stream state coverage (onRecvHeaders error path)
// ============================

TEST(Http2Connection, HeadersOnHalfClosedRemoteStreamIsStreamError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  HpackEncoder encoder(4096);

  // Client sends HEADERS with END_STREAM → stream 1 goes HalfClosedRemote
  {
    RawBytes headerBlock;
    encoder.encode(headerBlock, ":method", "GET");
    encoder.encode(headerBlock, ":path", "/");
    encoder.encode(headerBlock, ":scheme", "https");
    encoder.encode(headerBlock, ":authority", "localhost");

    RawBytes buf;
    WriteFrame(buf, FrameType::Headers, ComputeHeaderFrameFlags(true, true), 1,
               static_cast<uint32_t>(headerBlock.size()));
    buf.unchecked_append(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(headerBlock.data()), headerBlock.size()));

    auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size());
    auto res = conn.processInput(span);
    ASSERT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
  }
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Client sends HEADERS again on stream 1 (already HalfClosedRemote) → onRecvHeaders error
  {
    RawBytes headerBlock;
    encoder.encode(headerBlock, ":method", "POST");
    encoder.encode(headerBlock, ":path", "/again");
    encoder.encode(headerBlock, ":scheme", "https");
    encoder.encode(headerBlock, ":authority", "localhost");

    RawBytes buf;
    WriteFrame(buf, FrameType::Headers, ComputeHeaderFrameFlags(false, true), 1,
               static_cast<uint32_t>(headerBlock.size()));
    buf.unchecked_append(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(headerBlock.data()), headerBlock.size()));

    auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf.data()), buf.size());
    auto res = conn.processInput(span);
    // Stream error produces RST_STREAM in output
    EXPECT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
    ASSERT_TRUE(conn.hasPendingOutput());
    const auto out = conn.getPendingOutput();
    ASSERT_GE(out.size(), FrameHeader::kSize);
    const auto outHeader = ParseFrameHeader(out);
    EXPECT_EQ(outHeader.type, FrameType::RstStream);
  }
}

// ============================
// DATA/HEADERS FrameSizeError paths
// ============================

TEST(Http2Connection, DataFrameWithPaddedFlagButEmptyPayloadIsFrameSizeError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // First create stream 1 with a valid HEADERS frame
  HpackEncoder encoder(4096);
  RawBytes headerBlock;
  encoder.encode(headerBlock, ":method", "GET");
  encoder.encode(headerBlock, ":path", "/");
  encoder.encode(headerBlock, ":scheme", "https");
  encoder.encode(headerBlock, ":authority", "localhost");

  RawBytes headersBuf;
  WriteFrame(headersBuf, FrameType::Headers, ComputeHeaderFrameFlags(false, true), 1,
             static_cast<uint32_t>(headerBlock.size()));
  headersBuf.unchecked_append(
      std::span<const std::byte>(reinterpret_cast<const std::byte*>(headerBlock.data()), headerBlock.size()));

  auto hdrSpan = std::span<const std::byte>(reinterpret_cast<const std::byte*>(headersBuf.data()), headersBuf.size());
  auto resHdr = conn.processInput(hdrSpan);
  ASSERT_NE(resHdr.action, Http2Connection::ProcessResult::Action::Error);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Send DATA frame with PADDED flag but ZERO-length payload → FrameSizeError from ParseDataFrame
  FrameHeader header;
  header.length = 0;
  header.type = FrameType::Data;
  header.flags = FrameFlags::DataPadded;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, std::span<const std::byte>{});
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FrameSizeError);
}

TEST(Http2Connection, HeadersFrameWithPaddedFlagButEmptyPayloadIsFrameSizeError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Send HEADERS frame with PADDED flag but zero-length payload → FrameSizeError from ParseHeadersFrame
  FrameHeader header;
  header.length = 0;
  header.type = FrameType::Headers;
  header.flags = FrameFlags::HeadersEndHeaders | FrameFlags::HeadersPadded;
  header.streamId = 1;

  auto bytes = SerializeFrame(header, std::span<const std::byte>{});
  auto res = conn.processInput(bytes);
  EXPECT_EQ(res.action, Http2Connection::ProcessResult::Action::Error);
  EXPECT_EQ(res.errorCode, ErrorCode::FrameSizeError);
}

// ============================
// CONTINUATION completion: invalid stream state (L813) and endStream close (L826)
// ============================

TEST(Http2Connection, ContinuationOnHalfClosedRemoteStreamIsStreamError) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Step 1: Client sends HEADERS on stream 1 with END_HEADERS + END_STREAM → HalfClosedRemote
  HpackEncoder encoder(4096);
  RawBytes headerBlock;
  encoder.encode(headerBlock, ":method", "GET");
  encoder.encode(headerBlock, ":path", "/");
  encoder.encode(headerBlock, ":scheme", "https");
  encoder.encode(headerBlock, ":authority", "localhost");

  RawBytes hdr1Buf;
  WriteFrame(hdr1Buf, FrameType::Headers, ComputeHeaderFrameFlags(true, true), 1,
             static_cast<uint32_t>(headerBlock.size()));
  hdr1Buf.unchecked_append(
      std::span<const std::byte>(reinterpret_cast<const std::byte*>(headerBlock.data()), headerBlock.size()));

  auto hdr1Span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(hdr1Buf.data()), hdr1Buf.size());
  auto res1 = conn.processInput(hdr1Span);
  ASSERT_NE(res1.action, Http2Connection::ProcessResult::Action::Error);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Step 2: Client sends another HEADERS on stream 1 WITHOUT END_HEADERS → enters continuation mode
  // Stream 1 is HalfClosedRemote; onRecvHeaders will be deferred to CONTINUATION completion
  std::array<std::byte, 1> fragment = {std::byte{0x82}};  // index 2: ":method GET"
  RawBytes hdr2Buf;
  WriteFrame(hdr2Buf, FrameType::Headers, ComputeHeaderFrameFlags(false, false), 1,
             static_cast<uint32_t>(fragment.size()));
  hdr2Buf.unchecked_append(fragment);

  auto hdr2Span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(hdr2Buf.data()), hdr2Buf.size());
  auto res2 = conn.processInput(hdr2Span);
  ASSERT_NE(res2.action, Http2Connection::ProcessResult::Action::Error);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Step 3: Send CONTINUATION with END_HEADERS → completes header block →
  // onRecvHeaders fails on HalfClosedRemote stream → stream error (RST_STREAM)
  std::array<std::byte, 1> contFragment = {std::byte{0x84}};  // index 4: ":path /"
  RawBytes contBuf;
  WriteContinuationFrame(contBuf, 1, contFragment);

  auto contSpan = std::span<const std::byte>(reinterpret_cast<const std::byte*>(contBuf.data()), contBuf.size());
  auto res = conn.processInput(contSpan);
  // Stream error: RST_STREAM in output, not a connection error
  EXPECT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
  ASSERT_TRUE(conn.hasPendingOutput());
  const auto out = conn.getPendingOutput();
  ASSERT_GE(out.size(), FrameHeader::kSize);
  const auto outHeader = ParseFrameHeader(out);
  EXPECT_EQ(outHeader.type, FrameType::RstStream);
}

TEST(Http2Connection, ContinuationCompletionWithEndStreamClosesStream) {
  Http2Config config;
  Http2Connection conn(config, true);
  AdvanceToOpenAndDrainSettingsAck(conn);

  // Step 1: Client sends HEADERS on stream 1 with END_HEADERS (no END_STREAM) → Open
  HpackEncoder encoder(4096);
  RawBytes headerBlock;
  encoder.encode(headerBlock, ":method", "GET");
  encoder.encode(headerBlock, ":path", "/");
  encoder.encode(headerBlock, ":scheme", "https");
  encoder.encode(headerBlock, ":authority", "localhost");

  RawBytes hdr1Buf;
  WriteFrame(hdr1Buf, FrameType::Headers, ComputeHeaderFrameFlags(false, true), 1,
             static_cast<uint32_t>(headerBlock.size()));
  hdr1Buf.unchecked_append(
      std::span<const std::byte>(reinterpret_cast<const std::byte*>(headerBlock.data()), headerBlock.size()));

  auto hdr1Span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(hdr1Buf.data()), hdr1Buf.size());
  auto res1 = conn.processInput(hdr1Span);
  ASSERT_NE(res1.action, Http2Connection::ProcessResult::Action::Error);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Step 2: Server sends response headers with END_STREAM → stream becomes HalfClosedLocal
  ASSERT_EQ(conn.sendHeaders(1, http::StatusCodeOK, HeadersView{}, true), ErrorCode::NoError);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Step 3: Client sends HEADERS on stream 1 with END_STREAM but WITHOUT END_HEADERS
  // → enters continuation mode. Stream is HalfClosedLocal; onRecvHeaders(true) → Closed
  HpackEncoder encoder2(4096);
  RawBytes headerBlock2;
  encoder2.encode(headerBlock2, ":method", "GET");

  RawBytes hdr2Buf;
  WriteFrame(hdr2Buf, FrameType::Headers, ComputeHeaderFrameFlags(true, false), 1,
             static_cast<uint32_t>(headerBlock2.size()));
  hdr2Buf.unchecked_append(
      std::span<const std::byte>(reinterpret_cast<const std::byte*>(headerBlock2.data()), headerBlock2.size()));

  auto hdr2Span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(hdr2Buf.data()), hdr2Buf.size());
  auto res2 = conn.processInput(hdr2Span);
  ASSERT_NE(res2.action, Http2Connection::ProcessResult::Action::Error);
  if (conn.hasPendingOutput()) {
    conn.onOutputWritten(conn.getPendingOutput().size());
  }

  // Step 4: Send CONTINUATION with END_HEADERS to complete the block
  // onRecvHeaders(endStream=true) on HalfClosedLocal → Closed → closeStream called
  HpackEncoder encoder3(4096);
  RawBytes contHeaderBlock;
  encoder3.encode(contHeaderBlock, ":path", "/");

  RawBytes contBuf;
  WriteContinuationFrame(
      contBuf, 1,
      std::span<const std::byte>(reinterpret_cast<const std::byte*>(contHeaderBlock.data()), contHeaderBlock.size()));

  auto contSpan = std::span<const std::byte>(reinterpret_cast<const std::byte*>(contBuf.data()), contBuf.size());
  auto res = conn.processInput(contSpan);
  // Should succeed - stream transitions to Closed and gets cleaned up
  EXPECT_NE(res.action, Http2Connection::ProcessResult::Action::Error);
  // Stream 1 should be in closed state (kept briefly for late frames)
  const auto* stream = conn.getStream(1);
  ASSERT_NE(stream, nullptr);
  EXPECT_TRUE(stream->isClosed());
}

}  // namespace
}  // namespace aeronet::http2
