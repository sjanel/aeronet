// Unit coverage for the response parser's automatic-decompression install path (DecodeContext): decoding
// from the receive buffer / de-framed chunks straight into a borrowed buffer, dropping Content-Encoding,
// and surfacing decode failures as parse errors. Driven directly (no sockets) for precise control.
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/raw-chars.hpp"
#include "response-parser.hpp"

namespace aeronet {
namespace {

constexpr std::size_t kMax = 8UL * 1024UL * 1024UL;

// Reusable decode plumbing for one parse() call.
struct Decode {
  internal::RequestDecompressionState state;
  DecompressionConfig config;
  RawChars out;
  RawChars tmp;

  ResponseParser::DecodeContext ctx() { return {.state = &state, .config = &config, .out = &out, .tmp = &tmp}; }
};

std::string LengthFramed(std::string_view encodingName, std::string_view body) {
  std::string raw = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Encoding: ";
  raw.append(encodingName);
  raw.append("\r\nContent-Length: ");
  raw.append(std::to_string(body.size()));
  raw.append("\r\n\r\n");
  raw.append(body);
  return raw;
}

}  // namespace

TEST(ResponseParserDecompress, DecodesEachSupportedEncodingLengthFramed) {
  const std::string payload = test::MakePatternedPayload(4096);
  for (const Encoding enc : test::SupportedEncodings()) {
    const RawChars compressed = test::Compress(enc, payload);
    Decode decode;
    ResponseParser parser;
    parser.reset(false);
    parser.setDecodeContext(decode.ctx());
    HttpResponse resp;
    const std::string raw = LengthFramed(GetEncodingStr(enc), std::string_view(compressed));
    const auto st = parser.parse(raw, false, resp, kMax);
    EXPECT_EQ(st, ResponseParser::Status::Complete) << GetEncodingStr(enc);
    EXPECT_EQ(resp.bodyInMemory(), payload) << GetEncodingStr(enc);
    EXPECT_TRUE(resp.headerValueOrEmpty("content-encoding").empty()) << GetEncodingStr(enc);
    EXPECT_EQ(resp.headerValueOrEmpty("content-type"), "text/plain") << GetEncodingStr(enc);
  }
}

TEST(ResponseParserDecompress, DecodesChunkedCompressed) {
  if (test::SupportedEncodings().empty()) {
    GTEST_SKIP() << "no codec compiled in";
  }
  const Encoding enc = test::SupportedEncodings().front();
  const std::string payload = test::MakePatternedPayload(4096);
  const RawChars compressed = test::Compress(enc, payload);

  // Wrap the compressed bytes in a single chunk.
  std::string raw = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Encoding: ";
  raw.append(GetEncodingStr(enc));
  raw.append("\r\nTransfer-Encoding: chunked\r\n\r\n");
  char hex[32];
  raw.append(hex, to_lower_hex(compressed.size(), hex));
  raw.append(http::CRLF);
  raw.append(std::string_view(compressed));
  raw.append("\r\n0\r\n\r\n");

  Decode decode;
  ResponseParser parser;
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const auto st = parser.parse(raw, false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), payload);
  EXPECT_TRUE(resp.headerValueOrEmpty("content-encoding").empty());
}

TEST(ResponseParserDecompress, NoContentEncodingPassesThrough) {
  Decode decode;
  ResponseParser parser;
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const auto st = parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "hello");
}

TEST(ResponseParserDecompress, IdentityEncodingPassesThroughAndDropsHeader) {
  Decode decode;
  ResponseParser parser;
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const auto st = parser.parse(LengthFramed("identity", "hello"), false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "hello");
  EXPECT_TRUE(resp.headerValueOrEmpty("content-encoding").empty());
}

TEST(ResponseParserDecompress, UnsupportedEncodingIsError) {
  Decode decode;
  ResponseParser parser;
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const auto st = parser.parse(LengthFramed("made-up", "whatever-bytes"), false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserDecompress, GarbageCompressedBodyIsError) {
  if (test::SupportedEncodings().empty()) {
    GTEST_SKIP() << "no codec compiled in";
  }
  const Encoding enc = test::SupportedEncodings().front();
  Decode decode;
  ResponseParser parser;
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const auto st = parser.parse(LengthFramed(GetEncodingStr(enc), "not-a-valid-frame"), false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

}  // namespace aeronet
