#include "aeronet/http2-frame.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

#include "../src/http2-frame-type-str.hpp"
#include "aeronet/http2-error-code-name.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-process-result-error-msg-strings.hpp"
#include "aeronet/http2-process-result-error-msg.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/raw-bytes.hpp"

namespace aeronet::http2 {

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
  EXPECT_EQ(ErrorCodeName(static_cast<ErrorCode>(256U)), "UNKNOWN_ERROR");
}

// ============================
// Frame Header Tests
// ============================

TEST(Http2Frame, ParseFrameHeaderBasic) {
  // A minimal frame header: DATA frame, length 0, flags 0, stream 1
  const std::byte raw[]{
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00},                   // length: 0
      std::byte{0x00},                                                     // type: DATA
      std::byte{0x00},                                                     // flags: 0
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},  // stream ID: 1
  };

  FrameHeader header = ParseFrameHeader(raw);
  EXPECT_EQ(header.length(), 0);
  EXPECT_EQ(header.type(), FrameType::Data);
  EXPECT_EQ(header.flags(), 0);
  EXPECT_EQ(header.streamId(), 1U);
}

TEST(Http2Frame, ParseFrameHeaderWithLength) {
  // HEADERS frame, length 256, flags END_HEADERS, stream 3
  const std::byte raw[]{
      std::byte{0x00}, std::byte{0x01}, std::byte{0x00},                   // length: 256
      std::byte{0x01},                                                     // type: HEADERS
      std::byte{0x04},                                                     // flags: END_HEADERS
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x03},  // stream ID: 3
  };

  FrameHeader header = ParseFrameHeader(raw);
  EXPECT_EQ(header.length(), 256U);
  EXPECT_EQ(header.type(), FrameType::Headers);
  EXPECT_EQ(header.flags(), FrameFlags::HeadersEndHeaders);
  EXPECT_EQ(header.streamId(), 3U);
}

TEST(Http2Frame, WriteFrameHeader) {
  std::array<std::byte, 9> buffer;

  const FrameHeader header(100, FrameType::Data, FrameFlags::DataEndStream, 5);

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

  const FrameHeader header(5, FrameType::Data, FrameFlags::DataEndStream, 1);

  DataFrame frame;
  FrameParseResult result = ParseDataFrame(header, payload, frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_EQ(frame.data.size(), 5U);
  EXPECT_EQ(static_cast<char>(frame.data[0]), 'h');
}

TEST(Http2Frame, ParseDataFrameWithPadding) {
  // Padded data frame: pad_length=2, data="AB", padding=00 00
  constexpr std::byte payload[]{
      std::byte{0x02},                   // pad length
      std::byte{'A'},  std::byte{'B'},   // data
      std::byte{0x00}, std::byte{0x00},  // padding
  };

  const FrameHeader header(5, FrameType::Data, FrameFlags::DataPadded, 1);

  DataFrame frame;
  FrameParseResult result = ParseDataFrame(header, payload, frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_EQ(frame.data.size(), 2U);
  EXPECT_EQ(static_cast<char>(frame.data[0]), 'A');
  EXPECT_EQ(static_cast<char>(frame.data[1]), 'B');
}

TEST(Http2Frame, PrepareDataFrameGetStartPtr) {
  RawBytes buffer;
  const std::byte data[]{std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'}};

  std::byte* pData = PrepareDataFrameGetStartPtr(buffer, 1, static_cast<uint32_t>(std::size(data)), true);

  EXPECT_EQ(pData, buffer.end() - std::size(data));
  EXPECT_EQ(buffer.size(), FrameHeader::kSize + 5);

  // Parse it back
  auto span = std::span<const std::byte>(buffer);
  FrameHeader header = ParseFrameHeader(span);
  EXPECT_EQ(header.type(), FrameType::Data);
  EXPECT_EQ(header.length(), 5U);
  EXPECT_TRUE(header.hasFlag(FrameFlags::DataEndStream));
  EXPECT_EQ(header.streamId(), 1U);
}

TEST(Http2Frame, FrameHeaderMaxFieldValues) {
  const FrameHeader header(FrameHeader::kMaxLength, static_cast<FrameType>(0xFF), 0xFF, kMaxWindowSize);
  EXPECT_EQ(header.length(), FrameHeader::kMaxLength);
  EXPECT_EQ(header.type(), static_cast<FrameType>(0xFF));
  EXPECT_EQ(header.flags(), 0xFF);
  EXPECT_EQ(header.streamId(), kMaxWindowSize);

  std::array<std::byte, FrameHeader::kSize> buffer;
  WriteFrameHeader(buffer.data(), header);
  for (std::size_t pos = 0; pos < buffer.size(); ++pos) {
    EXPECT_EQ(buffer[pos], pos == 5 ? std::byte{0x7F} : std::byte{0xFF}) << "byte " << pos;
  }
  EXPECT_EQ(ParseFrameHeader(buffer), header);
}

TEST(Http2Frame, FrameHeaderFieldsDoNotOverlap) {
  // Each field set alone, the others being 0.
  static_assert(FrameHeader(0x123456, FrameType::Data, 0, 0) == FrameHeader(0x123456, FrameType::Data, 0, 0));
  static_assert(FrameHeader(0x123456, FrameType::Data, 0, 0).length() == 0x123456);
  static_assert(FrameHeader(0, FrameType::Continuation, 0, 0).type() == FrameType::Continuation);
  static_assert(FrameHeader(0, FrameType::Data, 0xA5, 0).flags() == 0xA5);
  static_assert(FrameHeader(0, FrameType::Data, 0, 0x12345678).streamId() == 0x12345678);

  const FrameHeader header(0x0A0B0C, FrameType::Settings, 0x5A, 0x01020304);
  EXPECT_EQ(header.length(), 0x0A0B0CU);
  EXPECT_EQ(header.type(), FrameType::Settings);
  EXPECT_EQ(header.flags(), 0x5A);
  EXPECT_EQ(header.streamId(), 0x01020304U);

  std::array<std::byte, FrameHeader::kSize> buffer;
  WriteFrameHeader(buffer.data(), header);
  const std::byte expected[]{
      std::byte{0x0A}, std::byte{0x0B}, std::byte{0x0C},                   // length
      std::byte{0x04},                                                     // type: SETTINGS
      std::byte{0x5A},                                                     // flags
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},  // stream ID
  };
  EXPECT_TRUE(std::ranges::equal(buffer, expected));
  EXPECT_EQ(ParseFrameHeader(buffer), header);
}

TEST(Http2Frame, FrameHeaderReservedBitIsIgnored) {
  // RFC 9113 §4.1: the reserved bit MUST remain unset when sending and MUST be ignored when receiving.
  const FrameHeader built(0, FrameType::Ping, FrameFlags::None, 0x80000007U);
  EXPECT_EQ(built.streamId(), 7U);

  std::array<std::byte, FrameHeader::kSize> buffer;
  WriteFrameHeader(buffer.data(), built);
  EXPECT_EQ(buffer[5], std::byte{0x00});
  EXPECT_EQ(buffer[8], std::byte{0x07});

  buffer[5] = std::byte{0x80};  // reserved bit set by the peer
  const FrameHeader parsed = ParseFrameHeader(buffer);
  EXPECT_EQ(parsed.streamId(), 7U);
  EXPECT_EQ(parsed, built);
}

// ============================
// HEADERS Frame Tests
// ============================

TEST(Http2Frame, ParseHeadersFrameSimple) {
  const std::byte payload[]{
      std::byte{0x82},  // Indexed header field: :method: GET
      std::byte{0x86},  // Indexed header field: :scheme: https
      std::byte{0x84},  // Indexed header field: :path: /
      std::byte{0x01},  // Indexed header field: :authority (index 1)
  };

  const FrameHeader header(4, FrameType::Headers, FrameFlags::HeadersEndHeaders | FrameFlags::HeadersEndStream, 1);

  HeadersFrame frame;
  uint32_t streamDependency{};
  FrameParseResult result = ParseHeadersFrame(header, payload, frame, streamDependency);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_EQ(frame.pHeaderBlockFragment, payload);
  EXPECT_EQ(frame.headerBlockFragmentSize, 4U);
}

TEST(Http2Frame, ParseHeadersFrameWithPriority) {
  const std::byte payload[]{
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},  // stream dependency
      std::byte{0xFF},                                                     // weight 255 on wire => 256 actual
      std::byte{0x82}, std::byte{0x86}, std::byte{0x84}, std::byte{0x01},  // header block
  };

  const FrameHeader header(9, FrameType::Headers, FrameFlags::HeadersEndHeaders | FrameFlags::HeadersPriority, 1);

  HeadersFrame frame;
  uint32_t streamDependency{};
  FrameParseResult result = ParseHeadersFrame(header, payload, frame, streamDependency);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_EQ(streamDependency, 0U);
  // RFC 9113 §5.3.1: "Add one to the value to obtain a weight between 1 and 256."
  EXPECT_EQ(frame.weightMinusOne, 255U);
  EXPECT_FALSE(frame.exclusive);
  EXPECT_EQ(frame.headerBlockFragment().data(), payload + 5);
  EXPECT_EQ(frame.headerBlockFragment().size(), 4U);
}

TEST(Http2Frame, ParseHeadersFrameWithPaddingAndExclusivePriority) {
  const std::byte payload[]{
      std::byte{0x01},                                                     // pad length
      std::byte{0x80}, std::byte{0x00}, std::byte{0x01}, std::byte{0x05},  // exclusive, stream dependency 261
      std::byte{0x0F},                                                     // weight 15 on wire => 16 actual
      std::byte{0x82}, std::byte{0x86},                                    // header block
      std::byte{0x00},                                                     // padding
  };

  const FrameHeader header(sizeof(payload), FrameType::Headers,
                           static_cast<uint8_t>(FrameFlags::HeadersPadded | FrameFlags::HeadersPriority), 7);

  HeadersFrame frame;
  uint32_t streamDependency{};
  ASSERT_EQ(ParseHeadersFrame(header, payload, frame, streamDependency), FrameParseResult::Ok);
  EXPECT_EQ(streamDependency, 261U);
  EXPECT_TRUE(frame.exclusive);
  EXPECT_EQ(frame.weightMinusOne, 15U);
  EXPECT_EQ(frame.pHeaderBlockFragment, payload + 6);
  EXPECT_EQ(frame.headerBlockFragmentSize, 2U);
}

TEST(Http2Frame, ParseHeadersFrameEmptyPayloadWithPaddedFlag) {
  // Empty payload but HeadersPadded flag set -> FrameSizeError
  std::span<const std::byte> payload = std::span<const std::byte>{};

  const FrameHeader header(0, FrameType::Headers, FrameFlags::HeadersPadded, 1);

  HeadersFrame frame;
  uint32_t streamDependency{};
  FrameParseResult result = ParseHeadersFrame(header, payload, frame, streamDependency);

  EXPECT_EQ(result, FrameParseResult::FrameSizeError);
}

TEST(Http2Frame, ParseHeadersFrameWithPadding) {
  // Padded headers: pad_length=2, header block "AB", padding 0x00 0x00
  const std::byte payload[]{
      std::byte{0x02},
      std::byte{'A'},
      std::byte{'B'},
      std::byte{0x00},
  };

  const FrameHeader header(4, FrameType::Headers, FrameFlags::HeadersPadded | FrameFlags::HeadersEndHeaders, 1);

  HeadersFrame frame;
  uint32_t streamDependency{};
  FrameParseResult result = ParseHeadersFrame(header, payload, frame, streamDependency);

  EXPECT_EQ(result, FrameParseResult::Ok);
  ASSERT_EQ(frame.headerBlockFragmentSize, 1U);  // 4 - 1(padlen) - 2(pad)
  EXPECT_EQ(frame.pHeaderBlockFragment[0], std::byte{'A'});
}

TEST(Http2Frame, WriteHeadersFrame) {
  RawBytes buffer;
  const std::byte headerBlock[]{
      std::byte{0x82},
      std::byte{0x86},
      std::byte{0x84},
  };

  WriteFrame(buffer, FrameType::Headers, ComputeHeaderFrameFlags(true, true), 1,
             static_cast<uint32_t>(sizeof(headerBlock)));
  buffer.append(headerBlock);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type(), FrameType::Headers);
  EXPECT_EQ(header.length(), 3U);
  EXPECT_TRUE(header.hasFlag(FrameFlags::HeadersEndStream));
  EXPECT_TRUE(header.hasFlag(FrameFlags::HeadersEndHeaders));
}

TEST(Http2Frame, ParseHeadersFrameWithPriorityTooShort) {
  // PRIORITY flag set, but only 4 bytes present (need 5: 4 for dependency + 1 for weight).
  const std::byte payload[]{
      std::byte{0x00},
      std::byte{0x00},
      std::byte{0x00},
      std::byte{0x00},
  };

  const FrameHeader header(static_cast<uint32_t>(sizeof(payload)), FrameType::Headers,
                           FrameFlags::HeadersPriority | FrameFlags::HeadersEndHeaders, 1);

  HeadersFrame frame;
  uint32_t streamDependency{};
  FrameParseResult result = ParseHeadersFrame(header, payload, frame, streamDependency);

  EXPECT_EQ(result, FrameParseResult::FrameSizeError);
}

TEST(Http2Frame, ParseHeadersFrameWithPaddingAndPriorityTooShort) {
  // PADDED + PRIORITY: offset=1 after pad-length byte, then only 4 more bytes
  // (need offset + 5 = 6, have 5) -> FrameSizeError.
  const std::byte payload[]{
      std::byte{0x00},                                                     // pad length
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},  // 4 bytes, missing weight byte
  };

  const FrameHeader header(
      static_cast<uint32_t>(sizeof(payload)), FrameType::Headers,
      static_cast<uint8_t>(FrameFlags::HeadersPadded | FrameFlags::HeadersPriority) | FrameFlags::HeadersEndHeaders, 1);

  HeadersFrame frame;
  uint32_t streamDependency{};
  FrameParseResult result = ParseHeadersFrame(header, payload, frame, streamDependency);

  EXPECT_EQ(result, FrameParseResult::FrameSizeError);
}

// ============================
// PRIORITY Frame Tests
// ============================

TEST(Http2Frame, ParsePriorityFrame) {
  const std::byte payload[]{
      std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0x03},  // exclusive, depends on 3
      std::byte{0x0F},                                                     // weight 15 on wire => 16 actual
  };

  PriorityFrame frame = ParsePriorityFrame(payload);

  EXPECT_TRUE(frame.exclusive);
  EXPECT_EQ(frame.streamDependency, 3U);
  // RFC 9113 §5.3.1: "Add one to the value to obtain a weight between 1 and 256."
  EXPECT_EQ(frame.weightMinusOne, 15U);
}

TEST(Http2Frame, WritePriorityFrame) {
  RawBytes buffer;
  WritePriorityFrame(buffer, 5, 3, 15, true);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type(), FrameType::Priority);
  EXPECT_EQ(header.length(), 5U);
  EXPECT_EQ(header.streamId(), 5U);
}

// ============================
// RST_STREAM Frame Tests
// ============================

TEST(Http2Frame, ParseRstStreamFrame) {
  const std::byte payload[]{
      std::byte{0x00},
      std::byte{0x00},
      std::byte{0x00},
      std::byte{0x08},  // CANCEL
  };

  RstStreamFrame frame;
  FrameParseResult result = ParseRstStreamFrame(payload, frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_EQ(frame.errorCode, ErrorCode::Cancel);
}

TEST(Http2Frame, WriteRstStreamFrame) {
  RawBytes buffer;
  WriteRstStreamFrame(buffer, 1, ErrorCode::Cancel);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type(), FrameType::RstStream);
  EXPECT_EQ(header.length(), 4U);
  EXPECT_EQ(header.streamId(), 1U);
}

// ============================
// SETTINGS Frame Tests
// ============================

TEST(Http2Frame, WriteSettingsFrame) {
  RawBytes buffer;
  const SettingsEntry entries[]{
      SettingsEntry{SettingsParameter::MaxConcurrentStreams, 100},
      SettingsEntry{SettingsParameter::InitialWindowSize, 65536},
  };

  WriteSettingsFrame(buffer, entries);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type(), FrameType::Settings);
  EXPECT_EQ(header.length(), 12U);
  EXPECT_EQ(header.streamId(), 0U);
}

TEST(Http2Frame, WriteSettingsAckFrame) {
  RawBytes buffer;
  WriteSettingsAckFrame(buffer);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type(), FrameType::Settings);
  EXPECT_EQ(header.length(), 0U);
  EXPECT_TRUE(header.hasFlag(FrameFlags::SettingsAck));
}

// ============================
// PING Frame Tests
// ============================

TEST(Http2Frame, ParsePingFrame) {
  const std::byte payload[]{
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
      std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
  };

  const FrameHeader header(8, FrameType::Ping, FrameFlags::PingAck, 0);

  PingFrame frame;
  FrameParseResult result = ParsePingFrame(header, payload, frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_TRUE(frame.isAck);
  EXPECT_EQ(frame.opaqueData[0], std::byte{0x01});
  EXPECT_EQ(frame.opaqueData[7], std::byte{0x08});
}

TEST(Http2Frame, WritePingFrame) {
  RawBytes buffer;
  PingFrame pingFrame;
  pingFrame.isAck = true;
  static constexpr std::byte opaqueData[]{
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
      std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
  };

  std::memcpy(pingFrame.opaqueData, opaqueData, sizeof(opaqueData));

  WritePingFrame(buffer, pingFrame);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type(), FrameType::Ping);
  EXPECT_EQ(header.length(), 8U);
  EXPECT_TRUE(header.hasFlag(FrameFlags::PingAck));
}

// ============================
// GOAWAY Frame Tests
// ============================

TEST(Http2Frame, ParseGoAwayFrame) {
  const std::byte payload[]{
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x05},                   // last stream ID: 5
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},                   // NO_ERROR
      std::byte{'t'},  std::byte{'e'},  std::byte{'s'},  std::byte{'t'},  std::byte{0x00},  // debug data
  };

  GoAwayFrame frame;
  FrameParseResult result = ParseGoAwayFrame(payload, frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_EQ(frame.lastStreamId, 5U);
  EXPECT_EQ(frame.errorCode, ErrorCode::NoError);
  EXPECT_EQ(frame.debugData.size(), 5U);
}

TEST(Http2Frame, WriteGoAwayFrame) {
  RawBytes buffer;
  WriteGoAwayFrame(buffer, 5, ErrorCode::NoError, ErrorMsg::NoError);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type(), FrameType::GoAway);
  EXPECT_EQ(header.length(), 16U);  // 8 + 8 bytes debug data
}

// ============================
// WINDOW_UPDATE Frame Tests
// ============================

TEST(Http2Frame, ParseWindowUpdateFrame) {
  const std::byte payload[]{
      std::byte{0x00},
      std::byte{0x00},
      std::byte{0x10},
      std::byte{0x00},  // increment: 4096
  };

  WindowUpdateFrame frame;
  FrameParseResult result = ParseWindowUpdateFrame(payload, frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_EQ(frame.windowSizeIncrement, 4096U);
}

TEST(Http2Frame, WriteWindowUpdateFrame) {
  RawBytes buffer;
  WriteWindowUpdateFrame(buffer, 0, 4096);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  EXPECT_EQ(header.type(), FrameType::WindowUpdate);
  EXPECT_EQ(header.length(), 4U);
  EXPECT_EQ(header.streamId(), 0U);
}

// ============================
// CONTINUATION Frame Tests
// ============================

TEST(Http2Frame, ParseContinuationFrame) {
  const std::byte payload[]{std::byte{0x82}, std::byte{0x86}, std::byte{0x84}};

  const FrameHeader header(3, FrameType::Continuation, FrameFlags::ContinuationEndHeaders, 1);

  ContinuationFrame frame;
  ParseContinuationFrame(header, payload, frame);

  EXPECT_TRUE(frame.endHeaders);
  EXPECT_EQ(frame.headerBlockFragment.size(), 3U);
}

// ============================
// Error Condition Tests
// ============================

TEST(Http2Frame, ParseDataFrameInvalidPadding) {
  const std::byte payload[]{
      std::byte{0x05},  // pad length: 5 (but only 1 byte of actual data)
      std::byte{'A'},
  };

  const FrameHeader header(2, FrameType::Data, FrameFlags::DataPadded, 1);

  DataFrame frame;
  FrameParseResult result = ParseDataFrame(header, payload, frame);

  EXPECT_EQ(result, FrameParseResult::InvalidPadding);
}

TEST(Http2Frame, ParseDataFrameEmptyPayloadWithPaddedFlag) {
  // payload empty but DataPadded flag set -> FrameSizeError
  std::span<const std::byte> payload = std::span<const std::byte>{};

  const FrameHeader header(0, FrameType::Data, FrameFlags::DataPadded, 1);

  DataFrame frame;
  FrameParseResult result = ParseDataFrame(header, payload, frame);

  EXPECT_EQ(result, FrameParseResult::FrameSizeError);
}

TEST(Http2Frame, ParsePingFrameInvalidLength) {
  const std::byte payload[]{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};

  const FrameHeader header(4, FrameType::Ping, 0, 0);

  PingFrame frame;
  FrameParseResult result = ParsePingFrame(header, payload, frame);

  EXPECT_EQ(result, FrameParseResult::FrameSizeError);
}

TEST(Http2Frame, ParseRstStreamFrameInvalidLength) {
  const std::byte payload[]{std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};

  RstStreamFrame frame;
  FrameParseResult result = ParseRstStreamFrame(payload, frame);

  EXPECT_EQ(result, FrameParseResult::FrameSizeError);
}

TEST(Http2Frame, ParseWindowUpdateFrameInvalidLength) {
  const std::byte payload[]{std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};

  WindowUpdateFrame frame;
  FrameParseResult result = ParseWindowUpdateFrame(payload, frame);

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

  std::byte* pData = PrepareDataFrameGetStartPtr(buffer, 7, static_cast<uint32_t>(data.size()), true);
  Copy(data.data(), data.size(), pData);

  auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size());
  FrameHeader header = ParseFrameHeader(span);

  DataFrame frame;
  FrameParseResult result = ParseDataFrame(header, span.subspan(FrameHeader::kSize), frame);

  EXPECT_EQ(result, FrameParseResult::Ok);
  EXPECT_EQ(frame.data.size(), 10U);
  for (int idx = 0; idx < 10; ++idx) {
    EXPECT_EQ(frame.data[static_cast<std::size_t>(idx)], static_cast<std::byte>(idx));
  }
}

TEST(Http2Frame, ErrorMsg) {
  using T = std::underlying_type_t<ErrorMsg>;
  static_assert(sizeof(T) < sizeof(uint32_t));

  for (uint32_t errorIdx = 0; errorIdx < std::numeric_limits<T>::max(); ++errorIdx) {
    ErrorMsg errorMsg = static_cast<ErrorMsg>(errorIdx);

    EXPECT_FALSE(ConvertProcessResultErrorMsgToSv(errorMsg).empty());
  }
}

TEST(Http2Frame, FrameTypeName) {
  EXPECT_EQ(FrameTypeName(FrameType::Data), "data");
  EXPECT_EQ(FrameTypeName(FrameType::Headers), "headers");
  EXPECT_EQ(FrameTypeName(FrameType::Priority), "priority");
  EXPECT_EQ(FrameTypeName(FrameType::RstStream), "rst_stream");
  EXPECT_EQ(FrameTypeName(FrameType::Settings), "settings");
  EXPECT_EQ(FrameTypeName(FrameType::PushPromise), "push_promise");
  EXPECT_EQ(FrameTypeName(FrameType::Ping), "ping");
  EXPECT_EQ(FrameTypeName(FrameType::GoAway), "goaway");
  EXPECT_EQ(FrameTypeName(FrameType::WindowUpdate), "window_update");
  EXPECT_EQ(FrameTypeName(FrameType::Continuation), "continuation");
  EXPECT_EQ(FrameTypeName(static_cast<FrameType>(0xFF)), "unknown");
}

}  // namespace aeronet::http2