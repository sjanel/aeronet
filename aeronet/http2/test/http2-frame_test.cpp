#include "aeronet/http2-frame.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>

#include "aeronet/http2-frame.hpp"
#include "aeronet/raw-bytes.hpp"

namespace aeronet::http2 {

namespace {

// Helper to create a span from raw bytes
template <std::size_t N>
std::span<const std::byte> AsSpan(const std::array<std::byte, N>& arr) {
  return std::span<const std::byte>(arr.data(), arr.size());
}

}  // namespace

TEST(Http2FrameNames, FrameTypeName_AllKnown) {
  EXPECT_EQ(FrameTypeName(FrameType::Data), "DATA");
  EXPECT_EQ(FrameTypeName(FrameType::Headers), "HEADERS");
  EXPECT_EQ(FrameTypeName(FrameType::Priority), "PRIORITY");
  EXPECT_EQ(FrameTypeName(FrameType::RstStream), "RST_STREAM");
  EXPECT_EQ(FrameTypeName(FrameType::Settings), "SETTINGS");
  EXPECT_EQ(FrameTypeName(FrameType::PushPromise), "PUSH_PROMISE");
  EXPECT_EQ(FrameTypeName(FrameType::Ping), "PING");
  EXPECT_EQ(FrameTypeName(FrameType::GoAway), "GOAWAY");
  EXPECT_EQ(FrameTypeName(FrameType::WindowUpdate), "WINDOW_UPDATE");
  EXPECT_EQ(FrameTypeName(FrameType::Continuation), "CONTINUATION");

  // Unknown extension value -> "UNKNOWN"
  EXPECT_EQ(FrameTypeName(static_cast<FrameType>(0x0A)), "UNKNOWN");
}

TEST(Http2FrameNames, ErrorCodeName_AllKnown) {
  EXPECT_EQ(ErrorCodeName(ErrorCode::NoError), "NO_ERROR");
  EXPECT_EQ(ErrorCodeName(ErrorCode::ProtocolError), "PROTOCOL_ERROR");
  EXPECT_EQ(ErrorCodeName(ErrorCode::InternalError), "INTERNAL_ERROR");
  EXPECT_EQ(ErrorCodeName(ErrorCode::FlowControlError), "FLOW_CONTROL_ERROR");
  EXPECT_EQ(ErrorCodeName(ErrorCode::SettingsTimeout), "SETTINGS_TIMEOUT");
  EXPECT_EQ(ErrorCodeName(ErrorCode::StreamClosed), "STREAM_CLOSED");
  EXPECT_EQ(ErrorCodeName(ErrorCode::FrameSizeError), "FRAME_SIZE_ERROR");
  EXPECT_EQ(ErrorCodeName(ErrorCode::RefusedStream), "REFUSED_STREAM");
  EXPECT_EQ(ErrorCodeName(ErrorCode::Cancel), "CANCEL");
  EXPECT_EQ(ErrorCodeName(ErrorCode::CompressionError), "COMPRESSION_ERROR");
  EXPECT_EQ(ErrorCodeName(ErrorCode::ConnectError), "CONNECT_ERROR");
  EXPECT_EQ(ErrorCodeName(ErrorCode::EnhanceYourCalm), "ENHANCE_YOUR_CALM");
  EXPECT_EQ(ErrorCodeName(ErrorCode::InadequateSecurity), "INADEQUATE_SECURITY");
  EXPECT_EQ(ErrorCodeName(ErrorCode::Http11Required), "HTTP_1_1_REQUIRED");

  // Unknown numeric value -> "UNKNOWN_ERROR"
  EXPECT_EQ(ErrorCodeName(static_cast<ErrorCode>(0xFF)), "UNKNOWN_ERROR");
}

// ============================
// Frame Header Tests
// ============================

TEST(Http2Frame, ParseFrameHeaderBasic) {
  // A minimal frame header: DATA frame, length 0, flags 0, stream 1
  std::array<std::byte, 9> raw = {
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00},                  // length: 0
      std::byte{0x00},                                                    // type: DATA
      std::byte{0x00},                                                    // flags: 0
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}  // stream ID: 1
  };

  FrameHeader header = ParseFrameHeader(AsSpan(raw));
  EXPECT_EQ(header.length, 0);
  EXPECT_EQ(header.type, FrameType::Data);
  EXPECT_EQ(header.flags, 0);
  EXPECT_EQ(header.streamId, 1U);
}

TEST(Http2Frame, ParseFrameHeaderWithLength) {
  // HEADERS frame, length 256, flags END_HEADERS, stream 3
  std::array<std::byte, 9> raw = {
      std::byte{0x00}, std::byte{0x01}, std::byte{0x00},                  // length: 256
      std::byte{0x01},                                                    // type: HEADERS
      std::byte{0x04},                                                    // flags: END_HEADERS
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x03}  // stream ID: 3
  };

  FrameHeader header = ParseFrameHeader(AsSpan(raw));
  EXPECT_EQ(header.length, 256U);
  EXPECT_EQ(header.type, FrameType::Headers);
  EXPECT_EQ(header.flags, FrameFlags::HeadersEndHeaders);
  EXPECT_EQ(header.streamId, 3U);
}

TEST(Http2Frame, WriteFrameHeader) {
  std::array<std::byte, 9> buffer;

  FrameHeader header{};
  header.length = 100;
  header.type = FrameType::Data;
  header.flags = FrameFlags::DataEndStream;
  header.streamId = 5;

  WriteFrameHeader(buffer.data(), header);

  // Verify the written bytes
  EXPECT_EQ(buffer[0], std::byte{0x00});
  EXPECT_EQ(buffer[1], std::byte{0x00});
  EXPECT_EQ(buffer[2], std::byte{0x64});  // 100 in hex
  EXPECT_EQ(buffer[3], std::byte{0x00});  // DATA
  EXPECT_EQ(buffer[4], std::byte{0x01});  // END_STREAM
  EXPECT_EQ(buffer[8], std::byte{0x05});  // stream ID low byte
}

// ============================
// DATA Frame Tests
// ============================

TEST(Http2Frame, ParseDataFrameSimple) {
  std::array<std::byte, 5> payload = {std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'}};

  FrameHeader header{};
  header.length = 5;
  header.type = FrameType::Data;
  header.flags = FrameFlags::DataEndStream;
  header.streamId = 1;

  DataFrame frame;
  FrameParseResult result = ParseDataFrame(header, AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_TRUE(frame.endStream);
  EXPECT_EQ(frame.data.size(), 5u);
  EXPECT_EQ(static_cast<char>(frame.data[0]), 'h');
}

TEST(Http2Frame, ParseDataFrameWithPadding) {
  // Padded data frame: pad_length=2, data="AB", padding=00 00
  std::array<std::byte, 5> payload = {
      std::byte{0x02},                  // pad length
      std::byte{'A'}, std::byte{'B'},   // data
      std::byte{0x00}, std::byte{0x00}  // padding
  };

  FrameHeader header{};
  header.length = 5;
  header.type = FrameType::Data;
  header.flags = FrameFlags::DataPadded;
  header.streamId = 1;

  DataFrame frame;
  FrameParseResult result = ParseDataFrame(header, AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_EQ(frame.padLength, 2U);
  EXPECT_EQ(frame.data.size(), 2U);
  EXPECT_EQ(static_cast<char>(frame.data[0]), 'A');
  EXPECT_EQ(static_cast<char>(frame.data[1]), 'B');
}

TEST(Http2Frame, WriteDataFrame) {
  RawBytes buffer;
  std::array<std::byte, 5> data = {std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'}};

  std::size_t written = WriteDataFrame(buffer, 1, data, true);

  EXPECT_EQ(written, FrameHeader::kSize + 5);
  EXPECT_EQ(buffer.size(), FrameHeader::kSize + 5);

  // Parse it back
  auto span = std::span<const std::byte>(buffer);
  FrameHeader header = ParseFrameHeader(span);
  EXPECT_EQ(header.type, FrameType::Data);
  EXPECT_EQ(header.length, 5U);
  EXPECT_TRUE(header.hasFlag(FrameFlags::DataEndStream));
  EXPECT_TRUE(header.isValid());
  EXPECT_EQ(header.streamId, 1U);
}

TEST(Http2Frame, InvalidLength) {
  FrameHeader header{};
  header.length = 1 << 26;

  EXPECT_FALSE(header.isValid());
}

// ============================
// HEADERS Frame Tests
// ============================

TEST(Http2Frame, ParseHeadersFrameSimple) {
  std::array<std::byte, 4> payload = {
      std::byte{0x82},  // Indexed header field: :method: GET
      std::byte{0x86},  // Indexed header field: :scheme: https
      std::byte{0x84},  // Indexed header field: :path: /
      std::byte{0x01}   // Indexed header field: :authority (index 1)
  };

  FrameHeader header{};
  header.length = 4;
  header.type = FrameType::Headers;
  header.flags = FrameFlags::HeadersEndHeaders | FrameFlags::HeadersEndStream;
  header.streamId = 1;

  HeadersFrame frame;
  FrameParseResult result = ParseHeadersFrame(header, AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_TRUE(frame.endHeaders);
  EXPECT_TRUE(frame.endStream);
  EXPECT_FALSE(frame.hasPriority);
  EXPECT_EQ(frame.headerBlockFragment.size(), 4U);
}

TEST(Http2Frame, ParseHeadersFrameWithPriority) {
  std::array<std::byte, 9> payload = {
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},  // stream dependency
      std::byte{0xFF},                                                     // weight 255 on wire => 256 actual
      std::byte{0x82}, std::byte{0x86}, std::byte{0x84}, std::byte{0x01}   // header block
  };

  FrameHeader header{};
  header.length = 9;
  header.type = FrameType::Headers;
  header.flags = FrameFlags::HeadersEndHeaders | FrameFlags::HeadersPriority;
  header.streamId = 1;

  HeadersFrame frame;
  FrameParseResult result = ParseHeadersFrame(header, AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_TRUE(frame.hasPriority);
  EXPECT_EQ(frame.streamDependency, 0U);
  // RFC 9113 §5.3.1: "Add one to the value to obtain a weight between 1 and 256."
  EXPECT_EQ(frame.weight, 256U);
  EXPECT_FALSE(frame.exclusive);
  EXPECT_EQ(frame.headerBlockFragment.size(), 4U);
}

TEST(Http2Frame, ParseHeadersFrameEmptyPayloadWithPaddedFlag) {
  // Empty payload but HeadersPadded flag set -> FrameSizeError
  std::span<const std::byte> payload = std::span<const std::byte>{};

  FrameHeader header{};
  header.length = 0;
  header.type = FrameType::Headers;
  header.flags = FrameFlags::HeadersPadded;
  header.streamId = 1;

  HeadersFrame frame;
  FrameParseResult result = ParseHeadersFrame(header, payload, frame);

  EXPECT_EQ(result, FrameParseResult::FrameSizeError);
}

TEST(Http2Frame, ParseHeadersFrameWithPadding) {
  // Padded headers: pad_length=2, header block "AB", padding 0x00 0x00
  std::array<std::byte, 4> payload = {std::byte{0x02}, std::byte{'A'}, std::byte{'B'}, std::byte{0x00}};

  FrameHeader header{};
  header.length = 4;
  header.type = FrameType::Headers;
  header.flags = FrameFlags::HeadersPadded | FrameFlags::HeadersEndHeaders;
  header.streamId = 1;

  HeadersFrame frame;
  FrameParseResult result = ParseHeadersFrame(header, AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_EQ(frame.padLength, 2U);
  EXPECT_EQ(frame.headerBlockFragment.size(), 1U);  // 4 - 1(padlen) - 2(pad)
}

TEST(Http2Frame, WriteHeadersFrame) {
  RawBytes buffer;
  std::array<std::byte, 3> headerBlock = {std::byte{0x82}, std::byte{0x86}, std::byte{0x84}};

  WriteFrame(buffer, FrameType::Headers, ComputeHeaderFrameFlags(true, true), 1, headerBlock.size());
  buffer.append(headerBlock);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type, FrameType::Headers);
  EXPECT_EQ(header.length, 3U);
  EXPECT_TRUE(header.hasFlag(FrameFlags::HeadersEndStream));
  EXPECT_TRUE(header.hasFlag(FrameFlags::HeadersEndHeaders));
}

// ============================
// PRIORITY Frame Tests
// ============================

TEST(Http2Frame, ParsePriorityFrame) {
  std::array<std::byte, 5> payload = {
      std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0x03},  // exclusive, depends on 3
      std::byte{0x0F}                                                      // weight 15 on wire => 16 actual
  };

  FrameHeader header{};
  header.length = 5;
  header.type = FrameType::Priority;
  header.flags = 0;
  header.streamId = 5;

  PriorityFrame frame;
  FrameParseResult result = ParsePriorityFrame(header, AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_TRUE(frame.exclusive);
  EXPECT_EQ(frame.streamDependency, 3U);
  // RFC 9113 §5.3.1: "Add one to the value to obtain a weight between 1 and 256."
  EXPECT_EQ(frame.weight, 16U);
}

TEST(Http2Frame, WritePriorityFrame) {
  RawBytes buffer;
  WritePriorityFrame(buffer, 5, 3, 15, true);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type, FrameType::Priority);
  EXPECT_EQ(header.length, 5U);
  EXPECT_EQ(header.streamId, 5U);
}

// ============================
// RST_STREAM Frame Tests
// ============================

TEST(Http2Frame, ParseRstStreamFrame) {
  std::array<std::byte, 4> payload = {
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x08}  // CANCEL
  };

  FrameHeader header{};
  header.length = 4;
  header.type = FrameType::RstStream;
  header.flags = 0;
  header.streamId = 1;

  RstStreamFrame frame;
  FrameParseResult result = ParseRstStreamFrame(header, AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_EQ(frame.errorCode, ErrorCode::Cancel);
}

TEST(Http2Frame, WriteRstStreamFrame) {
  RawBytes buffer;
  WriteRstStreamFrame(buffer, 1, ErrorCode::Cancel);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type, FrameType::RstStream);
  EXPECT_EQ(header.length, 4U);
  EXPECT_EQ(header.streamId, 1U);
}

// ============================
// SETTINGS Frame Tests
// ============================

TEST(Http2Frame, ParseSettingsFrameEmpty) {
  FrameHeader header{};
  header.length = 0;
  header.type = FrameType::Settings;
  header.flags = FrameFlags::SettingsAck;
  header.streamId = 0;

  SettingsFrame frame;
  FrameParseResult result = ParseSettingsFrame(header, std::span<const std::byte>{}, frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_TRUE(frame.isAck);
  EXPECT_EQ(frame.entryCount, 0);
}

TEST(Http2Frame, ParseSettingsFrameWithEntries) {
  std::array<std::byte, 12> payload = {
      std::byte{0x00}, std::byte{0x03},                                    // MAX_CONCURRENT_STREAMS
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x64},  // value: 100
      std::byte{0x00}, std::byte{0x04},                                    // INITIAL_WINDOW_SIZE
      std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}   // value: 65536
  };

  FrameHeader header{};
  header.length = 12;
  header.type = FrameType::Settings;
  header.flags = 0;
  header.streamId = 0;

  SettingsFrame frame;
  FrameParseResult result = ParseSettingsFrame(header, AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_FALSE(frame.isAck);
  EXPECT_EQ(frame.entryCount, 2U);
  EXPECT_EQ(frame.entries[0].id, SettingsParameter::MaxConcurrentStreams);
  EXPECT_EQ(frame.entries[0].value, 100U);
  EXPECT_EQ(frame.entries[1].id, SettingsParameter::InitialWindowSize);
  EXPECT_EQ(frame.entries[1].value, 65536U);
}

TEST(Http2Frame, WriteSettingsFrame) {
  RawBytes buffer;
  std::array<SettingsEntry, 2> entries = {SettingsEntry{SettingsParameter::MaxConcurrentStreams, 100},
                                          SettingsEntry{SettingsParameter::InitialWindowSize, 65536}};

  WriteSettingsFrame(buffer, entries);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type, FrameType::Settings);
  EXPECT_EQ(header.length, 12U);
  EXPECT_EQ(header.streamId, 0U);
}

TEST(Http2Frame, WriteSettingsAckFrame) {
  RawBytes buffer;
  WriteSettingsAckFrame(buffer);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type, FrameType::Settings);
  EXPECT_EQ(header.length, 0U);
  EXPECT_TRUE(header.hasFlag(FrameFlags::SettingsAck));
}

// ============================
// PING Frame Tests
// ============================

TEST(Http2Frame, ParsePingFrame) {
  std::array<std::byte, 8> payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
                                      std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};

  FrameHeader header{};
  header.length = 8;
  header.type = FrameType::Ping;
  header.flags = FrameFlags::PingAck;
  header.streamId = 0;

  PingFrame frame;
  FrameParseResult result = ParsePingFrame(header, AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_TRUE(frame.isAck);
  EXPECT_EQ(frame.opaqueData[0], std::byte{0x01});
  EXPECT_EQ(frame.opaqueData[7], std::byte{0x08});
}

TEST(Http2Frame, WritePingFrame) {
  RawBytes buffer;
  std::array<std::byte, 8> opaqueData = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
                                         std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};

  WritePingFrame(buffer, opaqueData, true);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type, FrameType::Ping);
  EXPECT_EQ(header.length, 8U);
  EXPECT_TRUE(header.hasFlag(FrameFlags::PingAck));
}

// ============================
// GOAWAY Frame Tests
// ============================

TEST(Http2Frame, ParseGoAwayFrame) {
  std::array<std::byte, 13> payload = {
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x05},                  // last stream ID: 5
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},                  // NO_ERROR
      std::byte{'t'},  std::byte{'e'},  std::byte{'s'},  std::byte{'t'},  std::byte{0x00}  // debug data
  };

  FrameHeader header{};
  header.length = 13;
  header.type = FrameType::GoAway;
  header.flags = 0;
  header.streamId = 0;

  GoAwayFrame frame;
  FrameParseResult result = ParseGoAwayFrame(header, AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_EQ(frame.lastStreamId, 5U);
  EXPECT_EQ(frame.errorCode, ErrorCode::NoError);
  EXPECT_EQ(frame.debugData.size(), 5U);
}

TEST(Http2Frame, WriteGoAwayFrame) {
  RawBytes buffer;
  WriteGoAwayFrame(buffer, 5, ErrorCode::NoError, "test");

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type, FrameType::GoAway);
  EXPECT_EQ(header.length, 12U);  // 8 + 4 bytes debug data
}

// ============================
// WINDOW_UPDATE Frame Tests
// ============================

TEST(Http2Frame, ParseWindowUpdateFrame) {
  std::array<std::byte, 4> payload = {
      std::byte{0x00}, std::byte{0x00}, std::byte{0x10}, std::byte{0x00}  // increment: 4096
  };

  WindowUpdateFrame frame;
  FrameParseResult result = ParseWindowUpdateFrame(AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_EQ(frame.windowSizeIncrement, 4096U);
}

TEST(Http2Frame, WriteWindowUpdateFrame) {
  RawBytes buffer;
  WriteWindowUpdateFrame(buffer, 0, 4096);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type, FrameType::WindowUpdate);
  EXPECT_EQ(header.length, 4U);
  EXPECT_EQ(header.streamId, 0U);
}

// ============================
// CONTINUATION Frame Tests
// ============================

TEST(Http2Frame, ParseContinuationFrame) {
  std::array<std::byte, 3> payload = {std::byte{0x82}, std::byte{0x86}, std::byte{0x84}};

  FrameHeader header{};
  header.length = 3;
  header.type = FrameType::Continuation;
  header.flags = FrameFlags::ContinuationEndHeaders;
  header.streamId = 1;

  ContinuationFrame frame;
  FrameParseResult result = ParseContinuationFrame(header, AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_TRUE(frame.endHeaders);
  EXPECT_EQ(frame.headerBlockFragment.size(), 3U);
}

TEST(Http2Frame, WriteContinuationFrame) {
  RawBytes buffer;
  std::array<std::byte, 3> headerBlock = {std::byte{0x82}, std::byte{0x86}, std::byte{0x84}};

  WriteContinuationFrame(buffer, 1, headerBlock, true);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type, FrameType::Continuation);
  EXPECT_EQ(header.length, 3U);
  EXPECT_TRUE(header.hasFlag(FrameFlags::ContinuationEndHeaders));
}

// ============================
// Error Condition Tests
// ============================

TEST(Http2Frame, ParseDataFrameInvalidPadding) {
  std::array<std::byte, 2> payload = {std::byte{0x05},  // pad length: 5 (but only 1 byte of actual data)
                                      std::byte{'A'}};

  FrameHeader header{};
  header.length = 2;
  header.type = FrameType::Data;
  header.flags = FrameFlags::DataPadded;
  header.streamId = 1;

  DataFrame frame;
  FrameParseResult result = ParseDataFrame(header, AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::InvalidPadding);
}

TEST(Http2Frame, ParseDataFrameEmptyPayloadWithPaddedFlag) {
  // payload empty but DataPadded flag set -> FrameSizeError
  std::span<const std::byte> payload = std::span<const std::byte>{};

  FrameHeader header{};
  header.length = 0;
  header.type = FrameType::Data;
  header.flags = FrameFlags::DataPadded;
  header.streamId = 1;

  DataFrame frame;
  FrameParseResult result = ParseDataFrame(header, payload, frame);

  EXPECT_EQ(result, FrameParseResult::FrameSizeError);
}

TEST(Http2Frame, ParseSettingsFrameInvalidLength) {
  // Settings entries must be 6 bytes each
  std::array<std::byte, 5> payload = {std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
                                      std::byte{0x10}};

  FrameHeader header{};
  header.length = 5;
  header.type = FrameType::Settings;
  header.flags = 0;
  header.streamId = 0;

  SettingsFrame frame;
  FrameParseResult result = ParseSettingsFrame(header, AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::FrameSizeError);
}

TEST(Http2Frame, ParsePingFrameInvalidLength) {
  std::array<std::byte, 4> payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};

  FrameHeader header{};
  header.length = 4;
  header.type = FrameType::Ping;
  header.flags = 0;
  header.streamId = 0;

  PingFrame frame;
  FrameParseResult result = ParsePingFrame(header, AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::FrameSizeError);
}

TEST(Http2Frame, ParseRstStreamFrameInvalidLength) {
  std::array<std::byte, 3> payload = {std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};

  FrameHeader header{};
  header.length = 3;
  header.type = FrameType::RstStream;
  header.flags = 0;
  header.streamId = 1;

  RstStreamFrame frame;
  FrameParseResult result = ParseRstStreamFrame(header, AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::FrameSizeError);
}

TEST(Http2Frame, ParsePriorityFrameInvalidLength) {
  std::array<std::byte, 4> payload = {std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};

  FrameHeader header{};
  header.length = 4;
  header.type = FrameType::Priority;
  header.flags = 0;
  header.streamId = 1;

  PriorityFrame frame;
  FrameParseResult result = ParsePriorityFrame(header, AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::FrameSizeError);
}

TEST(Http2Frame, ParseWindowUpdateFrameInvalidLength) {
  std::array<std::byte, 3> payload = {std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};

  WindowUpdateFrame frame;
  FrameParseResult result = ParseWindowUpdateFrame(AsSpan(payload), frame);

  EXPECT_EQ(result, FrameParseResult::FrameSizeError);
}

// ============================
// Round-trip Tests
// ============================

TEST(Http2Frame, RoundTripDataFrame) {
  RawBytes buffer;
  std::array<std::byte, 10> data;
  for (int idx = 0; idx < 10; ++idx) {
    data[static_cast<std::size_t>(idx)] = static_cast<std::byte>(idx);
  }

  WriteDataFrame(buffer, 7, data, true);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  DataFrame frame;
  FrameParseResult result = ParseDataFrame(header, span.subspan(FrameHeader::kSize), frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_TRUE(frame.endStream);
  EXPECT_EQ(frame.data.size(), 10U);
  for (int idx = 0; idx < 10; ++idx) {
    EXPECT_EQ(frame.data[static_cast<std::size_t>(idx)], static_cast<std::byte>(idx));
  }
}

TEST(Http2Frame, RoundTripSettingsFrame) {
  RawBytes buffer;
  std::array<SettingsEntry, 3> entries = {SettingsEntry{SettingsParameter::HeaderTableSize, 8192},
                                          SettingsEntry{SettingsParameter::MaxConcurrentStreams, 50},
                                          SettingsEntry{SettingsParameter::MaxFrameSize, 32768}};

  WriteSettingsFrame(buffer, entries);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  SettingsFrame frame;
  FrameParseResult result = ParseSettingsFrame(header, span.subspan(FrameHeader::kSize), frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_FALSE(frame.isAck);
  EXPECT_EQ(frame.entryCount, 3U);
  EXPECT_EQ(frame.entries[0].id, SettingsParameter::HeaderTableSize);
  EXPECT_EQ(frame.entries[0].value, 8192U);
  EXPECT_EQ(frame.entries[1].id, SettingsParameter::MaxConcurrentStreams);
  EXPECT_EQ(frame.entries[1].value, 50U);
  EXPECT_EQ(frame.entries[2].id, SettingsParameter::MaxFrameSize);
  EXPECT_EQ(frame.entries[2].value, 32768U);
}

}  // namespace aeronet::http2