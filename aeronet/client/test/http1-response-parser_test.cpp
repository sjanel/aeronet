#include "../src/http1-response-parser.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <limits>
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
#include "aeronet/response-body-sink.hpp"
#include "aeronet/toupperlower.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {
namespace {

constexpr std::size_t kMax = 1024UL * 1024UL;

Http1ResponseParser::Status ParseCopy(Http1ResponseParser& parser, std::string_view raw, bool eof, HttpResponse& resp,
                                      std::size_t maxBytes) {
  std::string mutableRaw(raw);
  return parser.parse(mutableRaw, eof, resp, maxBytes);
}

// Feed the whole buffer at once (eof=false unless stated). The parser borrows a body-assembly buffer; the
// tests below mirror this by declaring a local `RawChars bodyBuf;` right before each parser.
Http1ResponseParser::Status parseAll(std::string_view raw, HttpResponse& resp, bool head = false, bool eof = false,
                                     std::size_t maxBytes = kMax) {
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(head);
  return ParseCopy(parser, raw, eof, resp, maxBytes);
}

}  // namespace

TEST(ResponseParserTest, SimpleContentLength) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\ncontent-length: 5\r\n\r\nhello", resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.reason(), "OK");
  EXPECT_EQ(resp.bodyInMemory(), "hello");
}

TEST(ResponseParserTest, LargeIdentityBodyTransfersReceiveAllocationAndPreservesCapacity) {
  static constexpr std::size_t kBodySize = (1UL << 20U) + 123U;
  const std::string payload(kBodySize, 'x');
  std::string raw = "HTTP/1.1 200 OK\r\ncontent-type: application/test\r\ncontent-length: ";
  raw.append(std::to_string(payload.size()));
  raw.append("\r\n\r\n");
  const std::size_t bodyOffset = raw.size();
  raw.append(payload);

  RawChars bodyBuf;
  RawChars receiveBuffer(raw);
  const char* const receiveAllocation = receiveBuffer.data();
  const char* const bodyAllocation = receiveAllocation + bodyOffset;
  const std::size_t receiveCapacity = receiveBuffer.capacity();
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  HttpResponse first;
  ASSERT_EQ(parser.parse(receiveBuffer, false, first, raw.size()), Http1ResponseParser::Status::Complete);
  EXPECT_TRUE(first.hasBodyCaptured());
  EXPECT_EQ(first.bodyInMemory(), payload);
  EXPECT_EQ(first.bodyInMemory().data(), bodyAllocation);
  EXPECT_EQ(first.headerValueOrEmpty(http::ContentType), "application/test");
  EXPECT_TRUE(receiveBuffer.empty());
  EXPECT_EQ(receiveBuffer.capacity(), receiveCapacity);
  EXPECT_NE(receiveBuffer.data(), receiveAllocation);

  // The equal-capacity replacement remains client-owned. It retains its large allocation for a later response,
  // while a small body is copied so the response does not pin another high-water buffer.
  static constexpr std::string_view kSmallRaw = "HTTP/1.1 200 OK\r\ncontent-length: 5\r\n\r\nhello";
  const char* const replacementAllocation = receiveBuffer.data();
  receiveBuffer.assign(kSmallRaw);
  parser.reset(false);
  HttpResponse second;
  ASSERT_EQ(parser.parse(receiveBuffer, false, second, kMax), Http1ResponseParser::Status::Complete);
  EXPECT_FALSE(second.hasBodyCaptured());
  EXPECT_EQ(second.bodyInMemory(), "hello");
  EXPECT_TRUE(receiveBuffer.empty());
  EXPECT_EQ(receiveBuffer.data(), replacementAllocation);
  EXPECT_EQ(receiveBuffer.capacity(), receiveCapacity);
  EXPECT_EQ(first.bodyInMemory(), payload);

  // Captured-suffix payloads retain normal HttpResponse copy and append semantics.
  HttpResponse copy = first;
  EXPECT_EQ(copy.bodyInMemory(), payload);
  first.bodyAppend("tail");
  EXPECT_EQ(first.bodyInMemory().size(), payload.size() + 4U);
  EXPECT_TRUE(first.bodyInMemory().ends_with("tail"));
  EXPECT_EQ(copy.bodyInMemory(), payload);
}

TEST(ResponseParserTest, EmptyIdentityBodyKeepsReceiveScratchReusable) {
  static constexpr std::string_view kRaw = "HTTP/1.1 200 OK\r\ncontent-length: 0\r\n\r\n";
  RawChars bodyBuf;
  RawChars receiveBuffer(kRaw);
  const char* const receiveAllocation = receiveBuffer.data();
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  HttpResponse resp;
  ASSERT_EQ(parser.parse(receiveBuffer, false, resp, kMax), Http1ResponseParser::Status::Complete);
  EXPECT_TRUE(resp.bodyInMemory().empty());
  EXPECT_FALSE(resp.hasBodyCaptured());
  EXPECT_EQ(receiveBuffer.data(), receiveAllocation);
}

TEST(ResponseParserTest, StoresNonReservedHeader) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-MiXeD: value\r\nLocation: /x\r\n\r\nhi", resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.headerValueOrEmpty("location"), "/x");
  EXPECT_EQ(resp.headerValueOrEmpty("x-mixed"), "value");
  ASSERT_NE(resp.headers().begin(), resp.headers().end());
  EXPECT_EQ((*resp.headers().begin()).name, "x-mixed");
}

TEST(ResponseParserTest, ContentEncodingValueIsLowerCased) {
  // Content codings are case-insensitive: they are lower-cased when parsed (here without decoding, so the header is
  // kept, with a value long enough for the vectorized path), whereas other header values keep their case.
  HttpResponse resp;
  auto st = parseAll(
      "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Case: MiXeD\r\n"
      "Content-Encoding:  GZip, IDENTITY, Deflate, ZSTD, Br \r\n\r\nhi",
      resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), "gzip, identity, deflate, zstd, br");
  EXPECT_EQ(resp.headerValueOrEmpty("x-case"), "MiXeD");
  EXPECT_EQ(resp.bodyInMemory(), "hi");
}

TEST(ResponseParserTest, NoBodyOn204) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 204 No Content\r\n\r\n", resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.status(), 204);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST(ResponseParserTest, NoBodyOn304) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 304 Not Modified\r\n\r\n", resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.status(), 304);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST(ResponseParserTest, HeadRequestHasNoBodyDespiteContentLength) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\ncontent-length: 100\r\n\r\n", resp, /*head=*/true);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST(ResponseParserTest, RepeatedContentLengthHeader) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nhi", resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Error);
}

TEST(ResponseParserTest, ChunkedDecoding) {
  HttpResponse resp;
  std::string raw =
      "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n"
      "6\r\n world\r\n"
      "0\r\n\r\n";
  auto st = parseAll(raw, resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "hello world");
}

TEST(ResponseParserTest, ChunkedWithExtensionAndInvalidTrailer) {
  HttpResponse resp;
  std::string raw =
      "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n"
      "4;foo=bar\r\nabcd\r\n"
      "0\r\nx-trailer\r: v\r\n\r\n";
  auto st = parseAll(raw, resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Error);
}

TEST(ResponseParserTest, ChunkedWithExtensionAndTrailer) {
  HttpResponse resp;
  std::string raw =
      "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n"
      "4;foo=bar\r\nabcd\r\n"
      "0\r\nx-trailer: v\r\n\r\n";
  auto st = parseAll(raw, resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "abcd");
}

TEST(ResponseParserTest, ChunkSizeScannerAcceptsHexOwsAndExtensions) {
  HttpResponse resp;
  const std::string raw =
      "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n"
      "\t0A \t;foo=\"bar\tbaz\";flag\r\n"
      "0123456789\r\n"
      "0 ;done\r\n\r\n";
  EXPECT_EQ(parseAll(raw, resp), Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "0123456789");
}

TEST(ResponseParserTest, ChunkMetadataCanArriveOneCrlfByteAtATime) {
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n";

  EXPECT_EQ(parser.parse(raw, false, resp, kMax), Http1ResponseParser::Status::NeedMore);
  raw += "1;foo";
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), Http1ResponseParser::Status::NeedMore);
  raw.push_back('\r');
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), Http1ResponseParser::Status::NeedMore);
  raw += "\nx\r";
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), Http1ResponseParser::Status::NeedMore);
  raw += "\n0\r";
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), Http1ResponseParser::Status::NeedMore);
  raw += "\n\r\n";
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "x");
}

TEST(ResponseParserTest, UntilCloseFraming) {
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\n\r\nbody-bytes";
  auto st = parser.parse(raw, /*eof=*/false, resp, kMax);
  EXPECT_EQ(st, Http1ResponseParser::Status::NeedMore);
  st = parser.parse(raw, /*eof=*/true, resp, kMax);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "body-bytes");
  EXPECT_FALSE(parser.keepAlive());  // until-close cannot be reused
}

TEST(ResponseParserTest, IncrementalDelivery) {
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string buf;
  constexpr std::string_view chunks[]{"HTTP/1.1 200 OK\r\n", "content-length: 11\r\n\r\n", "hello", " world"};
  Http1ResponseParser::Status st = Http1ResponseParser::Status::NeedMore;
  for (auto chunk : chunks) {
    buf.append(chunk);
    st = parser.parse(buf, false, resp, kMax);
  }
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "hello world");
}

TEST(ResponseParserTest, KeepAliveDefaults) {
  RawChars bodyBuf;  // shared: the three parsers below run sequentially

  HttpResponse resp1;
  Http1ResponseParser p1(bodyBuf);
  p1.reset(false);
  ParseCopy(p1, "HTTP/1.1 200 OK\r\ncontent-length: 0\r\n\r\n", false, resp1, kMax);
  EXPECT_TRUE(p1.keepAlive());  // HTTP/1.1 default keep-alive

  HttpResponse resp2;
  Http1ResponseParser p2(bodyBuf);
  p2.reset(false);
  ParseCopy(p2, "HTTP/1.1 200 OK\r\ncontent-length: 0\r\nconnection: close\r\n\r\n", false, resp2, kMax);
  EXPECT_FALSE(p2.keepAlive());

  HttpResponse resp3;
  Http1ResponseParser p3(bodyBuf);
  p3.reset(false);
  ParseCopy(p3, "HTTP/1.0 200 OK\r\ncontent-length: 0\r\n\r\n", false, resp3, kMax);
  EXPECT_FALSE(p3.keepAlive());  // HTTP/1.0 default close
}

TEST(ResponseParserTest, DiscardsInterim100Continue) {
  HttpResponse resp;
  std::string raw = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\ncontent-length: 2\r\n\r\nok";
  auto st = parseAll(raw, resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "ok");
}

TEST(ResponseParserTest, RejectsGarbageStatusLine) {
  HttpResponse resp;
  auto st = parseAll("NOT-HTTP\r\n\r\n", resp, false, true);
  EXPECT_EQ(st, Http1ResponseParser::Status::Error);
}

TEST(ResponseParserTest, ConsumedTracksHeadPlusBody) {
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\ncontent-length: 3\r\n\r\nabcLEFTOVER";
  auto st = parser.parse(raw, false, resp, kMax);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "abc");
  EXPECT_EQ(parser.consumed(), raw.size() - std::string_view("LEFTOVER").size());
  EXPECT_FALSE(parser.keepAlive());  // bytes received after the end of the response
}

// Bytes received after the end of a response cannot be a response of their own: the connection is not reused, so that
// they are never taken for the response to the next request (RFC 9112 §6.3).
TEST(ResponseParserTest, BytesAfterTheResponseEndPreventReuse) {
  struct Case {
    std::string_view response;
    std::string_view body;
    bool head;
  };
  static constexpr Case kCases[] = {
      {"HTTP/1.1 200 OK\r\ncontent-length: 2\r\n\r\nhe", "he", false},
      {"HTTP/1.1 200 OK\r\ncontent-length: 0\r\n\r\n", "", false},
      {"HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n2\r\nhe\r\n0\r\n\r\n", "he", false},
      {"HTTP/1.1 204 No Content\r\n\r\n", "", false},
      {"HTTP/1.1 304 Not Modified\r\n\r\n", "", false},
      {"HTTP/1.1 200 OK\r\ncontent-length: 5\r\n\r\n", "", true},
      {"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\ncontent-length: 2\r\n\r\nhe", "he", false},
  };
  for (const Case& testCase : kCases) {
    for (const std::string_view extra : {std::string_view{}, std::string_view{"l"}, std::string_view{"llo"}}) {
      RawChars bodyBuf;
      Http1ResponseParser parser(bodyBuf);
      parser.reset(testCase.head);
      RawChars receive(testCase.response.size() + extra.size());
      receive.unchecked_append(testCase.response);
      receive.unchecked_append(extra);
      HttpResponse resp;
      ASSERT_EQ(parser.parse(receive, /*eof=*/false, resp, kMax), Http1ResponseParser::Status::Complete)
          << testCase.response;
      EXPECT_EQ(resp.bodyInMemory(), testCase.body) << testCase.response;
      EXPECT_EQ(parser.consumed(), testCase.response.size()) << testCase.response;
      EXPECT_EQ(parser.keepAlive(), extra.empty()) << testCase.response << extra;
    }
  }
}

TEST(ResponseParserTest, ContentLengthTruncatedAtEofIsTruncated) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\ncontent-length: 10\r\n\r\nabc", resp, /*head=*/false, /*eof=*/true);
  EXPECT_EQ(st, Http1ResponseParser::Status::Truncated);
}

TEST(ResponseParserTest, ChunkedIncompleteNeedsMore) {
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n5\r\nhel";
  auto st = parser.parse(raw, /*eof=*/false, resp, kMax);
  EXPECT_EQ(st, Http1ResponseParser::Status::NeedMore);
  raw += "lo\r\n0\r\n\r\n";
  st = parser.parse(raw, /*eof=*/false, resp, kMax);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "hello");
}

TEST(ResponseParserTest, MaxResponseBytesExceededIsTooLarge) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\ncontent-length: 100\r\n\r\n0123456789", resp, /*head=*/false, /*eof=*/false,
                     /*maxBytes=*/5);
  EXPECT_EQ(st, Http1ResponseParser::Status::TooLarge);
}

TEST(ResponseParserTest, InvalidContentLengthIsError) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\ncontent-length: abc\r\n\r\n", resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Error);
}

TEST(ResponseParserTest, InvalidCRLFIsError) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\rcontent-length: 4\r\n\r\nbody", resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Error);
}

TEST(ResponseParserTest, HeadersNeedMoreWhenIncomplete) {
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\nContent-Len";
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), Http1ResponseParser::Status::NeedMore);
  raw += "gth: 0\r";
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), Http1ResponseParser::Status::NeedMore);
  raw += "\n\r";
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), Http1ResponseParser::Status::NeedMore);
  raw.push_back('\n');
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), Http1ResponseParser::Status::Complete);
}

TEST(ResponseParserTest, Http10KeepAliveHeaderEnablesReuse) {
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  auto st =
      ParseCopy(parser, "HTTP/1.0 200 OK\r\nconnection: keep-alive\r\ncontent-length: 0\r\n\r\n", false, resp, kMax);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_TRUE(parser.keepAlive());  // explicit keep-alive overrides the HTTP/1.0 close default
}

TEST(ResponseParserTest, ConnectionTokenListClosesReuse) {
  // "close" appears as one token of a comma-separated Connection option list (RFC 9110 §7.6.1).
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  auto st =
      ParseCopy(parser, "HTTP/1.1 200 OK\r\nconnection: close, foo\r\ncontent-length: 0\r\n\r\n", false, resp, kMax);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_FALSE(parser.keepAlive());
  // The Connection header value is still preserved under its normalized name.
  EXPECT_EQ(resp.headerValueOrEmpty("connection"), "close, foo");
}

TEST(ResponseParserTest, Http10ConnectionTokenListEnablesReuse) {
  // "keep-alive" buried in a token list still overrides the HTTP/1.0 close default.
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  auto st = ParseCopy(parser, "HTTP/1.0 200 OK\r\nconnection: Keep-Alive, Upgrade\r\ncontent-length: 0\r\n\r\n", false,
                      resp, kMax);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_TRUE(parser.keepAlive());
}

TEST(ResponseParserTest, LengthBodyExceedsMaxIsTooLarge) {
  HttpResponse resp;
  // Headers fit under maxBytes, but the declared (and delivered) body overflows it.
  std::string raw = "HTTP/1.1 200 OK\r\ncontent-length: 100\r\n\r\n" + std::string(100, 'a');
  auto st = parseAll(raw, resp, /*head=*/false, /*eof=*/false, /*maxBytes=*/50);
  EXPECT_EQ(st, Http1ResponseParser::Status::TooLarge);
}

TEST(ResponseParserTest, UntilCloseBodyExceedsMaxIsTooLarge) {
  HttpResponse resp;
  std::string raw = "HTTP/1.1 200 OK\r\n\r\n" + std::string(100, 'b');
  auto st = parseAll(raw, resp, /*head=*/false, /*eof=*/false, /*maxBytes=*/50);
  EXPECT_EQ(st, Http1ResponseParser::Status::TooLarge);
}

TEST(ResponseParserTest, ChunkedBadSizeIsError) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\nXYZ\r\ndata", resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Error);
}

TEST(ResponseParserTest, ChunkedBodyExceedsMaxIsTooLarge) {
  HttpResponse resp;
  std::string raw =
      "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n64\r\n" + std::string(100, 'c') + "\r\n0\r\n\r\n";
  auto st = parseAll(raw, resp, /*head=*/false, /*eof=*/false, /*maxBytes=*/50);
  EXPECT_EQ(st, Http1ResponseParser::Status::TooLarge);
}

TEST(ResponseParserTest, ChunkedLoweredLimitRejectsAlreadyBufferedBody) {
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n2\r\nx";

  EXPECT_EQ(parser.parse(raw, false, resp, kMax), Http1ResponseParser::Status::NeedMore);
  ASSERT_EQ(bodyBuf.size(), 1);
  EXPECT_EQ(parser.parse(raw, false, resp, 0), Http1ResponseParser::Status::TooLarge);
}

TEST(ResponseParserTest, ChunkedMissingCrlfAtEofIsTruncated) {
  HttpResponse resp;
  auto st =
      parseAll("HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n5\r\nhello", resp, /*head=*/false, /*eof=*/true);
  EXPECT_EQ(st, Http1ResponseParser::Status::Truncated);
}

TEST(ResponseParserTest, ChunkedTrailersTruncatedAtEofIsTruncated) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n0\r\n", resp, /*head=*/false, /*eof=*/true);
  EXPECT_EQ(st, Http1ResponseParser::Status::Truncated);
}

TEST(ResponseParserTest, HeaderLineWithoutNewlineExceedingMaxIsTooLarge) {
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  auto st = ParseCopy(parser, "HTTP/1.1 200 OK\r\nincomplete-no-newline", /*eof=*/false, resp, 10);
  EXPECT_EQ(st, Http1ResponseParser::Status::TooLarge);
}

// A still-incomplete header line whose START is within the limit must still be bounded by the *buffered*
// size, otherwise a peer that streams an endless un-terminated header grows the receive buffer without
// limit (the line start never advances, so a `_pos`-only check would never trip).
TEST(ResponseParserTest, IncompleteHeaderBoundedByBufferedSize) {
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  // Status line (17 bytes) fits under maxBytes=50, but the unterminated header pushes the buffer past it.
  std::string raw = "HTTP/1.1 200 OK\r\nx: " + std::string(100, 'a');  // no trailing CRLF
  auto st = parser.parse(raw, /*eof=*/false, resp, /*maxResponseBytes=*/50);
  EXPECT_EQ(st, Http1ResponseParser::Status::TooLarge);
}

// A single oversized but newline-terminated header is rejected (the status-line / empty-line branches used
// to bypass the per-header size check, so such a head could be accepted whole).
TEST(ResponseParserTest, OversizedTerminatedHeaderIsTooLarge) {
  HttpResponse resp;
  const std::string raw = "HTTP/1.1 200 OK\r\nx: " + std::string(100, 'a') + "\r\n\r\n";
  auto st = parseAll(raw, resp, /*head=*/false, /*eof=*/false, /*maxBytes=*/50);
  EXPECT_EQ(st, Http1ResponseParser::Status::TooLarge);
}

TEST(ResponseParserTest, RejectsBadVersionToken) {
  HttpResponse resp;
  auto st = parseAll("HTTPX 200 OK\r\n\r\n", resp, false, true);
  EXPECT_EQ(st, Http1ResponseParser::Status::Error);
}

TEST(ResponseParserTest, RejectsBadVersionDigits) {
  HttpResponse resp;
  auto st = parseAll("HTTP/X.1 200 OK\r\n\r\n", resp, false, true);
  EXPECT_EQ(st, Http1ResponseParser::Status::Error);
}

// RFC 9112 §2.3: the version of the status line must be "HTTP/" DIGIT "." DIGIT (case-sensitive), of major version 1.
TEST(ResponseParserTest, RejectsNonHttp1Version) {
  for (std::string_view statusLine : {
           "HTTP/2.0 200 OK",
           "HTTP/0.9 200 OK",
           "XTTP/1.1 200 OK",
           "http/1.1 200 OK",
           "HTTP-1.1 200 OK",
           "HTTP/1x1 200 OK",
           "HTTP/1.x 200 OK",
           "HTTP/1.10 200 OK",
           "HTTP/1.1200 OK",
           "HTTP/1.1",
           "HTTP/1. 200 OK",
           " HTTP/1.1 200 OK",
       }) {
    SCOPED_TRACE(statusLine);
    HttpResponse resp;
    std::string raw(statusLine);
    raw.append("\r\ncontent-length: 0\r\n\r\n");
    EXPECT_EQ(parseAll(raw, resp, false, true), Http1ResponseParser::Status::Error);
  }
}

// RFC 9110 §6.2: a higher HTTP/1 minor version is processed as HTTP/1.1 - keep-alive by default.
TEST(ResponseParserTest, HigherHttp1MinorVersionIsProcessedAsHttp11) {
  for (std::string_view version : {"HTTP/1.2", "HTTP/1.9"}) {
    SCOPED_TRACE(version);
    RawChars bodyBuf;
    Http1ResponseParser parser(bodyBuf);
    parser.reset(false);
    HttpResponse resp;
    std::string raw(version);
    raw.append(" 200 OK\r\ncontent-length: 2\r\n\r\nok");
    EXPECT_EQ(ParseCopy(parser, raw, false, resp, kMax), Http1ResponseParser::Status::Complete);
    EXPECT_EQ(resp.status(), 200);
    EXPECT_EQ(resp.bodyInMemory(), "ok");
    EXPECT_TRUE(parser.keepAlive());
  }
}

TEST(ResponseParserTest, RejectsHeaderWithoutColon) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nnocolon\r\n\r\n", resp, false, true);
  EXPECT_EQ(st, Http1ResponseParser::Status::Error);
}

TEST(ResponseParserTest, HeaderLineBeyondMaxBytesIsTooLarge) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\na: 1\r\nb: 2\r\n\r\n", resp, /*head=*/false, /*eof=*/false, /*maxBytes=*/20);
  EXPECT_EQ(st, Http1ResponseParser::Status::TooLarge);
}

TEST(ResponseParserTest, ReparseAfterCompleteStaysComplete) {
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\ncontent-length: 0\r\n\r\n";
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), Http1ResponseParser::Status::Complete);
  // A redundant parse() once Done short-circuits straight back to Complete.
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), Http1ResponseParser::Status::Complete);
}

// "transfer-encoding: gzip, chunked" is chunked: only the *last* token decides framing. The chunk bytes
// here are plain text (no codec wired in), so de-chunking yields them verbatim.
TEST(ResponseParserTest, TransferEncodingCommaListLastTokenChunked) {
  HttpResponse resp;
  std::string raw =
      "HTTP/1.1 200 OK\r\ntransfer-encoding: gzip, chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n";
  auto st = parseAll(raw, resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "hello");
}

// Without chunked as final coding, the body ends with the connection (RFC 9112 §6.3), whatever Content-Length says.
// The coding list goes on over repeated fields: only the last one decides.
TEST(ResponseParserTest, TransferEncodingWithoutFinalChunkedIsDelimitedByClose) {
  for (const std::string_view head : {
           "transfer-encoding: gzip\r\ncontent-length: 2\r\n",
           "content-length: 2\r\ntransfer-encoding: chunked, gzip\r\n",
           "transfer-encoding: chunked\r\ntransfer-encoding: gzip\r\n",
           "transfer-encoding: gzip\r\n",
       }) {
    HttpResponse resp;
    RawChars bodyBuf;
    Http1ResponseParser parser(bodyBuf);
    parser.reset(false);
    std::string raw = "HTTP/1.1 200 OK\r\n" + std::string(head) + "\r\nhello";
    EXPECT_EQ(parser.parse(raw, /*eof=*/false, resp, kMax), Http1ResponseParser::Status::NeedMore) << head;
    EXPECT_EQ(parser.parse(raw, /*eof=*/true, resp, kMax), Http1ResponseParser::Status::Complete) << head;
    EXPECT_EQ(resp.bodyInMemory(), "hello") << head;
    EXPECT_FALSE(parser.keepAlive()) << head;
  }
}

// Chunked framing wins over Content-Length (RFC 9112 §6.3), but the connection is not reused after a response with both
// (possible response splitting, §11.1).
TEST(ResponseParserTest, ChunkedWithContentLengthIsNotReused) {
  constexpr std::string_view kChunkedBody = "5\r\nhello\r\n0\r\n\r\n";
  for (const bool withContentLength : {false, true}) {
    HttpResponse resp;
    RawChars bodyBuf;
    Http1ResponseParser parser(bodyBuf);
    parser.reset(false);
    const std::string raw = std::string("HTTP/1.1 200 OK\r\n") + (withContentLength ? "content-length: 3\r\n" : "") +
                            "transfer-encoding: chunked\r\n\r\n" + std::string(kChunkedBody);
    EXPECT_EQ(ParseCopy(parser, raw, /*eof=*/false, resp, kMax), Http1ResponseParser::Status::Complete);
    EXPECT_EQ(resp.bodyInMemory(), "hello");
    EXPECT_EQ(parser.keepAlive(), !withContentLength);
    EXPECT_EQ(parser.consumed(), raw.size());
  }
}

// An HTTP/1.0 response cannot be transfer-coded: its framing is faulty, even with a Content-Length (RFC 9112 §6.1). Its
// body ends with the connection, which is not reused despite its keep-alive.
TEST(ResponseParserTest, Http10TransferEncodingIsDelimitedByClose) {
  constexpr std::string_view kChunkedBody = "5\r\nhello\r\n0\r\n\r\n";
  for (const std::string_view head : {
           "transfer-encoding: chunked\r\n",
           "transfer-encoding: chunked\r\ncontent-length: 5\r\n",
       }) {
    HttpResponse resp;
    RawChars bodyBuf;
    Http1ResponseParser parser(bodyBuf);
    parser.reset(false);
    std::string raw =
        "HTTP/1.0 200 OK\r\nconnection: keep-alive\r\n" + std::string(head) + "\r\n" + std::string(kChunkedBody);
    EXPECT_EQ(parser.parse(raw, /*eof=*/false, resp, kMax), Http1ResponseParser::Status::NeedMore) << head;
    EXPECT_EQ(parser.parse(raw, /*eof=*/true, resp, kMax), Http1ResponseParser::Status::Complete) << head;
    EXPECT_EQ(resp.bodyInMemory(), kChunkedBody) << head;
    EXPECT_FALSE(parser.keepAlive()) << head;
  }
}

// The framing headers of a discarded interim response do not apply to the final one.
TEST(ResponseParserTest, InterimTransferEncodingDoesNotFrameFinalResponse) {
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  auto st = ParseCopy(parser,
                      "HTTP/1.1 103 Early Hints\r\ntransfer-encoding: gzip\r\ncontent-length: 7\r\n\r\n"
                      "HTTP/1.1 200 OK\r\ncontent-length: 2\r\n\r\nok",
                      /*eof=*/false, resp, kMax);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "ok");
  EXPECT_TRUE(parser.keepAlive());
}

// A chunk-size token with a valid hex prefix followed by trailing junk ("5x") is rejected (the parse
// stops before consuming the whole token).
TEST(ResponseParserTest, ChunkedSizeWithTrailingJunkIsError) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n5x\r\nhello", resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Error);
}

// A truncated chunk-size line (no terminating LF) that reaches EOF is reported, not an endless wait.
TEST(ResponseParserTest, ChunkedSizeTruncatedAtEofIsTruncated) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n3", resp, /*head=*/false, /*eof=*/true);
  EXPECT_EQ(st, Http1ResponseParser::Status::Truncated);
}

// Content-Length with a valid digit prefix then trailing junk ("5x") is rejected.
TEST(ResponseParserTest, ContentLengthWithTrailingJunkIsError) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\ncontent-length: 5x\r\n\r\n", resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Error);
}

// A header line whose first character is ':' (empty field name) is malformed.
TEST(ResponseParserTest, RejectsHeaderStartingWithColon) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\n:novalue\r\n\r\n", resp, /*head=*/false, /*eof=*/true);
  EXPECT_EQ(st, Http1ResponseParser::Status::Error);
}

TEST(ResponseParserTest, ChunkSizeOverflowIsRejectedWithoutRejectingSizeTMax) {
  constexpr std::size_t kMaxHexDigits = sizeof(std::size_t) * 2U;
  const std::string head = "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n";

  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  HttpResponse maxResponse;
  std::string maxLine = head + std::string(kMaxHexDigits, 'f') + "\r\n";
  EXPECT_EQ(parser.parse(maxLine, false, maxResponse, std::numeric_limits<std::size_t>::max()),
            Http1ResponseParser::Status::NeedMore);

  HttpResponse overflowResponse;
  const std::string overflowLine = head + std::string(kMaxHexDigits + 1U, 'f') + "\r\n";
  EXPECT_EQ(parseAll(overflowLine, overflowResponse), Http1ResponseParser::Status::Error);
}

TEST(ResponseParserTest, ChunkSizeScannerRejectsMalformedSyntax) {
  const auto expectError = [](std::string_view chunkLine) {
    HttpResponse resp;
    std::string raw = "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n";
    raw.append(chunkLine);
    raw.append("x\r\n0\r\n\r\n");
    EXPECT_EQ(parseAll(raw, resp), Http1ResponseParser::Status::Error) << chunkLine;
  };

  expectError("10 0\r\n");
  expectError(";foo\r\n");
  expectError(" \t\r\n");
  expectError("\r\n");
  expectError("1\rX\n");
  expectError("1;foo\rX\n");

  std::string controlExtension = "1;foo";
  controlExtension.push_back('\x01');
  controlExtension.append("bar\r\n");
  expectError(controlExtension);

  std::string deleteExtension = "1;foo";
  deleteExtension.push_back('\x7f');
  deleteExtension.append("bar\r\n");
  expectError(deleteExtension);
}

// Status codes that parse to a value but leave trailing junk, or fall outside the valid 100..599 range,
// are all rejected.
TEST(ResponseParserTest, RejectsStatusCodeTrailingJunkOrOutOfRange) {
  static constexpr std::string_view kBadStatusCodeResponseLines[]{
      "HTTP/1.1 20x OK\r\n\r\n",   // trailing junk after digits
      "HTTP/1.1 600 X\r\n\r\n",    // above 599
      "HTTP/1.1 abc OK\r\n\r\n",   // non-numeric
      "HTTP/1.1 99 OK\r\n\r\n",    // below 100
      "HTTP/1.1 0200 OK\r\n\r\n",  // != 3 DIGIT
  };
  for (const std::string_view raw : kBadStatusCodeResponseLines) {
    HttpResponse resp;
    EXPECT_EQ(parseAll(raw, resp, /*head=*/false, /*eof=*/true), Http1ResponseParser::Status::Error) << raw;
  }
}

TEST(ResponseParserTest, RejectsBareLfLineEndings) {
  HttpResponse resp;
  EXPECT_EQ(parseAll("HTTP/1.1 200 OK\nContent-Length: 2\n\nhi", resp, false, true),
            Http1ResponseParser::Status::Error);
}

TEST(ResponseParserTest, RejectsBareLfInChunkedBody) {
  constexpr std::string_view kHead = "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n";
  for (const std::string_view body : {
           std::string_view("5\nhello\r\n0\r\n\r\n"),
           std::string_view("5\r\nhello\n0\r\n\r\n"),
           std::string_view("5\r\nhello\r\n0\n\n"),
           std::string_view("0\r\n\n"),
       }) {
    HttpResponse resp;
    const std::string raw = std::string(kHead) + std::string(body);
    EXPECT_EQ(parseAll(raw, resp, false, true), Http1ResponseParser::Status::Error) << body;
  }
}

// A trailing comma in a Connection option list leaves an empty final token: the scanner must consume the
// list to the end (the loop re-checks its `!value.empty()` guard rather than exiting through the comma).
TEST(ResponseParserTest, ConnectionTrailingCommaClosesReuse) {
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  auto st = ParseCopy(parser, "HTTP/1.1 200 OK\r\nconnection: close,\r\ncontent-length: 0\r\n\r\n", false, resp, kMax);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_FALSE(parser.keepAlive());
}

// A chunk-size position holding a bare, empty line (no digits at all) is malformed: the empty token fails
// the hex parse rather than being mistaken for a zero-length final chunk.
TEST(ResponseParserTest, ChunkedEmptySizeLineIsError) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n\n", resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Error);
}

// Chunk data cut short (fewer bytes than the announced chunk size) at EOF is reported, not a silent wait.
TEST(ResponseParserTest, ChunkedDataTruncatedAtEofIsTruncated) {
  HttpResponse resp;
  auto st =
      parseAll("HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n5\r\nhel", resp, /*head=*/false, /*eof=*/true);
  EXPECT_EQ(st, Http1ResponseParser::Status::Truncated);
}

// Chunk data fully delivered but the trailing CRLF not yet arrived (and no EOF): the parser waits for more.
TEST(ResponseParserTest, ChunkedDataCrlfPendingNeedsMore) {
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n5\r\nhello";
  auto st = parser.parse(raw, /*eof=*/false, resp, kMax);
  EXPECT_EQ(st, Http1ResponseParser::Status::NeedMore);
  raw += "\r\n0\r\n\r\n";
  st = parser.parse(raw, /*eof=*/false, resp, kMax);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "hello");
}

TEST(ResponseParserTest, ChunkedBodyTransfersAllocationAndPreservesScratchCapacity) {
  const std::string payload(4096, 'x');
  std::string raw = "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n1000\r\n";
  raw.append(payload);

  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  HttpResponse first;
  ASSERT_EQ(parser.parse(raw, false, first, kMax), Http1ResponseParser::Status::NeedMore);
  const char* firstAllocation = bodyBuf.data();
  const std::size_t scratchCapacity = bodyBuf.capacity();
  ASSERT_NE(firstAllocation, nullptr);
  ASSERT_EQ(bodyBuf.size(), payload.size());

  raw.append("\r\n0\r\n\r\n");
  ASSERT_EQ(parser.parse(raw, false, first, kMax), Http1ResponseParser::Status::Complete);
  EXPECT_TRUE(first.hasBodyCaptured());
  EXPECT_EQ(first.bodyInMemory(), payload);
  EXPECT_EQ(first.bodyInMemory().data(), firstAllocation);
  EXPECT_TRUE(bodyBuf.empty());
  EXPECT_EQ(bodyBuf.capacity(), scratchCapacity);
  EXPECT_NE(bodyBuf.data(), firstAllocation);

  // The replacement is used directly by the next exchange, while the first response remains independent.
  const std::string secondPayload(2048, 'y');
  std::string secondRaw = "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n800\r\n";
  secondRaw.append(secondPayload);
  secondRaw.append("\r\n0\r\n\r\n");
  const char* secondAllocation = bodyBuf.data();
  parser.reset(false);
  HttpResponse second;
  ASSERT_EQ(parser.parse(secondRaw, false, second, kMax), Http1ResponseParser::Status::Complete);
  EXPECT_EQ(second.bodyInMemory(), secondPayload);
  EXPECT_EQ(second.bodyInMemory().data(), secondAllocation);
  EXPECT_EQ(first.bodyInMemory(), payload);
}

TEST(ResponseParserTest, ChunkDataDelimiterIsValidated) {
  constexpr std::string_view kHead = "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n";
  for (const std::string_view body : {std::string_view("1\r\nxX\n0\r\n\r\n"), std::string_view("1\r\nx\rX0\r\n\r\n")}) {
    HttpResponse resp;
    const std::string raw = std::string(kHead) + std::string(body);
    EXPECT_EQ(parseAll(raw, resp, false, true), Http1ResponseParser::Status::Error) << body;
  }

  HttpResponse truncated;
  const std::string trailingCr = std::string(kHead) + "1\r\nx\r";
  EXPECT_EQ(parseAll(trailingCr, truncated, false, true), Http1ResponseParser::Status::Truncated);
}

TEST(ResponseParserTest, OversizedScratchStaysReusableForSmallChunkedBody) {
  RawChars bodyBuf(4096);
  const char* scratchAllocation = bodyBuf.data();
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  HttpResponse resp;
  ASSERT_EQ(ParseCopy(parser, "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n", false,
                      resp, kMax),
            Http1ResponseParser::Status::Complete);
  EXPECT_FALSE(resp.hasBodyCaptured());
  EXPECT_EQ(resp.bodyInMemory(), "hello");
  EXPECT_NE(resp.bodyInMemory().data(), scratchAllocation);
  EXPECT_TRUE(bodyBuf.empty());
  EXPECT_EQ(bodyBuf.data(), scratchAllocation);
  EXPECT_EQ(bodyBuf.capacity(), 4096);
}

// A terminating zero-length chunk whose trailer block is not yet closed (and no EOF): keep waiting.
TEST(ResponseParserTest, ChunkedTrailersPendingNeedsMore) {
  HttpResponse resp;
  RawChars bodyBuf;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n0\r\n";
  auto st = parser.parse(raw, /*eof=*/false, resp, kMax);
  EXPECT_EQ(st, Http1ResponseParser::Status::NeedMore);
  raw += "\r\n";  // close the (empty) trailer block
  st = parser.parse(raw, /*eof=*/false, resp, kMax);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

// A status line with no reason phrase ("HTTP/1.1 200") is accepted: the code is parsed and the (absent)
// reason is simply left empty.
TEST(ResponseParserTest, StatusLineWithoutReasonPhrase) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200\r\ncontent-length: 0\r\n\r\n", resp);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.reason().empty());
}

namespace {

constexpr std::size_t kMaxResponseBytes = 8UL * 1024UL * 1024UL;

// Reusable decode plumbing for one parse() call.
struct Decode {
  DecompressionState state;
  DecompressionConfig config;
  RawChars out;
  RawChars tmp;
  RawChars bodyBuf;  // borrowed by the Http1ResponseParser for chunked reassembly

  Http1ResponseParser::DecodeContext ctx() {
    return {.pState = &state, .pConfig = &config, .pOut = &out, .pTmp = &tmp};
  }
};

std::string LengthFramed(std::string_view encodingName, std::string_view body) {
  std::string raw = "HTTP/1.1 200 OK\r\ncontent-type: text/plain\r\ncontent-encoding: ";
  raw.append(encodingName);
  raw.append("\r\ncontent-length: ");
  raw.append(std::to_string(body.size()));
  raw.append("\r\n\r\n");
  raw.append(body);
  return raw;
}

}  // namespace

TEST(ResponseParserDecompress, DecodesEachSupportedEncodingLengthFramed) {
  const std::string payload = test::MakePatternedPayload(64UL * 1024UL);
  for (const Encoding enc : test::SupportedEncodings()) {
    const RawChars compressed = test::Compress(enc, payload);
    Decode decode;
    decode.config.maxExpansionRatio = 1000;
    const std::size_t decodeScratchCapacity = payload.size();
    decode.tmp = RawChars(decodeScratchCapacity);
    const char* decodedAllocation = decode.tmp.data();
    Http1ResponseParser parser(decode.bodyBuf);
    parser.reset(false);
    parser.setDecodeContext(decode.ctx());
    HttpResponse resp;
    std::string raw = LengthFramed(GetEncodingStr(enc), std::string_view(compressed));
    const auto st = parser.parse(raw, false, resp, kMaxResponseBytes);
    EXPECT_EQ(st, Http1ResponseParser::Status::Complete) << GetEncodingStr(enc);
    EXPECT_EQ(resp.bodyInMemory(), payload) << GetEncodingStr(enc);
    EXPECT_TRUE(resp.hasBodyCaptured()) << GetEncodingStr(enc);
    EXPECT_EQ(resp.bodyInMemory().data(), decodedAllocation) << GetEncodingStr(enc);
    EXPECT_EQ(decode.out.capacity(), decodeScratchCapacity) << GetEncodingStr(enc);
    EXPECT_NE(decode.out.data(), decodedAllocation) << GetEncodingStr(enc);
    EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentEncoding).empty()) << GetEncodingStr(enc);
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/plain") << GetEncodingStr(enc);
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
  std::string raw = "HTTP/1.1 200 OK\r\ncontent-type: text/plain\r\ncontent-encoding: ";
  raw.append(GetEncodingStr(enc));
  raw.append("\r\ntransfer-encoding: chunked\r\n\r\n");
  char hex[32];
  raw.append(hex, ToLowerHex(compressed.size(), hex));
  raw.append(http::CRLF);
  raw.append(std::string_view(compressed));
  raw.append("\r\n0\r\n\r\n");

  Decode decode;
  Http1ResponseParser parser(decode.bodyBuf);
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const auto st = parser.parse(raw, false, resp, kMaxResponseBytes);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), payload);
  EXPECT_TRUE(resp.headerValueOrEmpty("content-encoding").empty());
}

TEST(ResponseParserDecompress, MixedCaseCodingsAreDecoded) {
  const std::string payload = test::MakePatternedPayload(4096);
  for (const Encoding enc : test::SupportedEncodings()) {
    const RawChars compressed = test::Compress(enc, payload);
    std::string coding(GetEncodingStr(enc));
    std::ranges::transform(coding, coding.begin(), [](char ch) { return toupper(ch); });
    coding.insert(0, "Identity, ");
    Decode decode;
    Http1ResponseParser parser(decode.bodyBuf);
    parser.reset(false);
    parser.setDecodeContext(decode.ctx());
    HttpResponse resp;
    std::string raw = LengthFramed(coding, std::string_view(compressed));
    EXPECT_EQ(parser.parse(raw, false, resp, kMaxResponseBytes), Http1ResponseParser::Status::Complete) << coding;
    EXPECT_EQ(resp.bodyInMemory(), payload) << coding;
    EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentEncoding).empty()) << coding;
  }
}

TEST(ResponseParserDecompress, NoContentEncodingPassesThrough) {
  Decode decode;
  Http1ResponseParser parser(decode.bodyBuf);
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const auto st =
      ParseCopy(parser, "HTTP/1.1 200 OK\r\ncontent-length: 5\r\n\r\nhello", false, resp, kMaxResponseBytes);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "hello");
}

TEST(ResponseParserDecompress, IdentityEncodingPassesThroughAndDropsHeader) {
  Decode decode;
  Http1ResponseParser parser(decode.bodyBuf);
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const std::string payload(256, 'i');
  const std::string raw = LengthFramed("identity", payload);
  const std::size_t bodyOffset = raw.size() - payload.size();
  RawChars receiveBuffer(raw);
  const char* const bodyAllocation = receiveBuffer.data() + bodyOffset;
  const auto st = parser.parse(receiveBuffer, false, resp, kMaxResponseBytes);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), payload);
  EXPECT_TRUE(resp.hasBodyCaptured());
  EXPECT_EQ(resp.bodyInMemory().data(), bodyAllocation);
  EXPECT_TRUE(receiveBuffer.empty());
  EXPECT_TRUE(resp.headerValueOrEmpty("content-encoding").empty());
}

TEST(ResponseParserDecompress, IdentityChunkedTransfersReassemblyAllocation) {
  Decode decode;
  decode.bodyBuf = RawChars(8);
  const char* bodyAllocation = decode.bodyBuf.data();
  Http1ResponseParser parser(decode.bodyBuf);
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const auto st = ParseCopy(
      parser,
      "HTTP/1.1 200 OK\r\ncontent-encoding: identity\r\ntransfer-encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
      false, resp, kMaxResponseBytes);
  EXPECT_EQ(st, Http1ResponseParser::Status::Complete);
  EXPECT_TRUE(resp.hasBodyCaptured());
  EXPECT_EQ(resp.bodyInMemory(), "hello");
  EXPECT_EQ(resp.bodyInMemory().data(), bodyAllocation);
  EXPECT_EQ(decode.bodyBuf.capacity(), 8);
  EXPECT_NE(decode.bodyBuf.data(), bodyAllocation);
  EXPECT_TRUE(resp.headerValueOrEmpty("content-encoding").empty());
}

TEST(ResponseParserDecompress, UnsupportedEncodingIsError) {
  Decode decode;
  Http1ResponseParser parser(decode.bodyBuf);
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const auto st = ParseCopy(parser, LengthFramed("made-up", "whatever-bytes"), false, resp, kMaxResponseBytes);
  EXPECT_EQ(st, Http1ResponseParser::Status::Error);
}

TEST(ResponseParserDecompress, GarbageCompressedBodyIsError) {
  if (test::SupportedEncodings().empty()) {
    GTEST_SKIP() << "no codec compiled in";
  }
  const Encoding enc = test::SupportedEncodings().front();
  Decode decode;
  Http1ResponseParser parser(decode.bodyBuf);
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const auto st =
      ParseCopy(parser, LengthFramed(GetEncodingStr(enc), "not-a-valid-frame"), false, resp, kMaxResponseBytes);
  EXPECT_EQ(st, Http1ResponseParser::Status::Error);
}

// Any non-Connection header name containing a byte outside the HTTP token charset (RFC 9110 §5.1) is
// rejected.
TEST(ResponseParserTest, RejectsInvalidHeaderName) {
  HttpResponse spaceResp;
  EXPECT_EQ(parseAll("HTTP/1.1 200 OK\r\nX Bad: value\r\n\r\n", spaceResp, /*head=*/false, /*eof=*/true),
            Http1ResponseParser::Status::Error);

  HttpResponse controlResp;
  std::string raw = "HTTP/1.1 200 OK\r\nX";
  raw.push_back('\x01');
  raw.append("Bad: value\r\n\r\n");
  EXPECT_EQ(parseAll(raw, controlResp, /*head=*/false, /*eof=*/true), Http1ResponseParser::Status::Error);
}

// Any non-Connection header value containing a disallowed control byte is rejected. This exercises the new
// IsValidHeaderValue check (previously a no-op for every header, Connection included).
TEST(ResponseParserTest, RejectsInvalidHeaderValue) {
  HttpResponse controlResp;
  std::string rawControl = "HTTP/1.1 200 OK\r\nX-Test: bad";
  rawControl.push_back('\x01');
  rawControl.append("value\r\n\r\n");
  EXPECT_EQ(parseAll(rawControl, controlResp, /*head=*/false, /*eof=*/true), Http1ResponseParser::Status::Error);

  HttpResponse delResp;
  std::string rawDel = "HTTP/1.1 200 OK\r\nX-Test: bad";
  rawDel.push_back('\x7f');
  rawDel.append("value\r\n\r\n");
  EXPECT_EQ(parseAll(rawDel, delResp, /*head=*/false, /*eof=*/true), Http1ResponseParser::Status::Error);

  HttpResponse obsTextControlResp;
  EXPECT_EQ(parseAll("HTTP/1.1 200 OK\r\nX-Test: caf\xC3\xA9\x01\r\n\r\n", obsTextControlResp, /*head=*/false,
                     /*eof=*/true),
            Http1ResponseParser::Status::Error);
}

// RFC 9110 §5.5: obs-text (bytes 0x80-0xFF, such as UTF-8 text) is allowed in header values, and kept as is.
TEST(ResponseParserTest, AcceptsObsTextInHeaderValue) {
  HttpResponse resp;
  EXPECT_EQ(parseAll("HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename=\"r\xC3\xA9sum\xC3\xA9.pdf\"\r\n"
                     "X-Latin1: \xE9t\xE9\r\nX-Bytes: \x80\xFF\r\nContent-Length: 2\r\n\r\nok",
                     resp),
            Http1ResponseParser::Status::Complete);
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.headerValueOrEmpty("content-disposition"), "attachment; filename=\"r\xC3\xA9sum\xC3\xA9.pdf\"");
  EXPECT_EQ(resp.headerValueOrEmpty("x-latin1"), "\xE9t\xE9");
  EXPECT_EQ(resp.headerValueOrEmpty("x-bytes"), "\x80\xFF");
  EXPECT_EQ(resp.bodyInMemory(), "ok");
}

// The Connection header is no longer exempt from the general header-value grammar: ScanConnectionTokens
// only decides which *recognized* connection-options (close / keep-alive) apply — an unrecognized-but-valid
// token like "Upgrade" is still ignored (see Http10ConnectionTokenListEnablesReuse) — but a value containing
// a byte outside the header-value grammar is a malformed message like any other invalid header value.
TEST(ResponseParserTest, RejectsInvalidConnectionHeaderValue) {
  HttpResponse resp;
  std::string raw = "HTTP/1.1 200 OK\r\nconnection: close,";
  raw.push_back('\x01');
  raw.append("\r\ncontent-length: 0\r\n\r\n");
  EXPECT_EQ(parseAll(raw, resp, /*head=*/false, /*eof=*/true), Http1ResponseParser::Status::Error);
}

// --- Streaming (reset with streamHead) ---

namespace {

// A piece of streamed body.
struct StreamedPiece {
  std::size_t size;
  bool gathered;  // small chunks gathered in the reassembly buffer, instead of a view into the receive buffer

  bool operator==(const StreamedPiece&) const = default;
};

// Drives a streaming parse of `raw` delivered `step` bytes at a time (eof after the last piece), like the HTTP/1.1
// engine does: collects the streamed body and returns the final status.
struct StreamedParse {
  Http1ResponseParser::Status status{Http1ResponseParser::Status::NeedMore};
  std::string body;
  std::size_t maxReceiveBufferSize{0};  // after discarding the streamed bytes
  std::size_t maxParsedBufferSize{0};   // when parsed, so with the received bytes
  int headCompleteCount{0};
  std::string contentType;
  std::size_t announcedBodySize{0};
  vector<StreamedPiece> pieces;
  bool keepAlive{false};
};

StreamedParse StreamParse(std::string_view raw, std::size_t step, std::size_t maxBytes = kMax, bool stream = true) {
  StreamedParse out;
  RawChars bodyBuf;
  RawChars receive;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false, /*streamHead=*/true);
  HttpResponse resp;
  for (std::size_t off = 0;;) {
    const std::size_t take = std::min(step, raw.size() - off);
    receive.append(raw.substr(off, take));
    off += take;
    const bool eof = off == raw.size();
    out.maxParsedBufferSize = std::max<std::size_t>(out.maxParsedBufferSize, receive.size());
    out.status = parser.parse(receive, eof, resp, maxBytes);
    if (out.status == Http1ResponseParser::Status::HeadComplete) {
      ++out.headCompleteCount;
      out.contentType = parser.contentType(receive);
      out.announcedBodySize = parser.announcedBodySize();
      if (stream) {
        parser.startStreaming();
      }
      out.status = parser.parse(receive, eof, resp, maxBytes);
    }
    while (out.status == Http1ResponseParser::Status::BodyPiece) {
      // Each piece is either a view into the receive buffer, or the whole reassembly buffer holding gathered chunks.
      const std::string_view piece = parser.streamedBody(receive);
      EXPECT_FALSE(piece.empty());
      const bool gathered = piece.data() == bodyBuf.data() && piece.size() == bodyBuf.size();
      EXPECT_TRUE(gathered ||
                  (piece.data() >= receive.data() && piece.data() + piece.size() <= receive.data() + receive.size()));
      out.pieces.push_back({.size = piece.size(), .gathered = gathered});
      out.body.append(piece);
      out.status = parser.parse(receive, eof, resp, maxBytes);
    }
    if (parser.streaming()) {
      parser.discardStreamed(receive);
    }
    out.maxReceiveBufferSize = std::max<std::size_t>(out.maxReceiveBufferSize, receive.size());
    if (out.status != Http1ResponseParser::Status::NeedMore || eof) {
      if (!stream && out.status == Http1ResponseParser::Status::Complete) {
        // A body received whole is left in the scratch buffers, not installed in the response.
        EXPECT_TRUE(resp.bodyInMemory().empty());
        out.body.assign(parser.bufferedBody());
      }
      out.keepAlive = parser.keepAlive();
      return out;
    }
  }
}

}  // namespace

TEST(ResponseParserStreamingTest, ContentLengthBodyIsStreamedAndNotBounded) {
  const std::string payload = test::MakePatternedPayload(100000);
  const std::string raw =
      "HTTP/1.1 200 OK\r\ncontent-type: text/csv\r\ncontent-length: " + std::to_string(payload.size()) + "\r\n\r\n" +
      payload;
  // maxBytes only bounds the head: the 100 kB body goes through.
  const StreamedParse res = StreamParse(raw, 4096, /*maxBytes=*/256);
  EXPECT_EQ(res.status, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(res.headCompleteCount, 1);
  EXPECT_EQ(res.contentType, "text/csv");
  EXPECT_EQ(res.announcedBodySize, payload.size());
  EXPECT_EQ(res.body, payload);
  EXPECT_LE(res.maxReceiveBufferSize, 4096U);  // consumed bytes are discarded at each step
}

TEST(ResponseParserStreamingTest, ChunkedBodyIsStreamedWithTrailers) {
  std::string payload;
  std::string raw = "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n";
  for (std::size_t idx = 0; idx < 50U; ++idx) {
    const std::string chunk = test::MakePatternedPayload(997U + idx);
    payload += chunk;
    char sizeBuf[32];
    const int len = std::snprintf(sizeBuf, sizeof(sizeBuf), "%zx;ext=1\r\n", chunk.size());
    raw.append(sizeBuf, static_cast<std::size_t>(len)).append(chunk).append("\r\n");
  }
  raw += "0\r\nx-checksum: abc\r\n\r\n";
  for (const std::size_t step : {std::size_t{1}, std::size_t{7}, std::size_t{1500}, raw.size()}) {
    const StreamedParse res = StreamParse(raw, step, /*maxBytes=*/512);
    EXPECT_EQ(res.status, Http1ResponseParser::Status::Complete) << step;
    EXPECT_EQ(res.body, payload) << step;
    EXPECT_TRUE(res.contentType.empty());
    EXPECT_EQ(res.announcedBodySize, ResponseBodyInfo::kUnknownSize) << step;
    // Small pieces are gathered, but never beyond what the receive buffer holds.
    for (const StreamedPiece& piece : res.pieces) {
      EXPECT_LE(piece.size, res.maxParsedBufferSize) << step;
    }
    EXPECT_LE(res.maxReceiveBufferSize, std::max<std::size_t>(step, 1100U)) << step;
  }
}

TEST(ResponseParserStreamingTest, UntilCloseBodyIsStreamed) {
  const std::string payload = test::MakePatternedPayload(20000);
  const StreamedParse res = StreamParse("HTTP/1.1 200 OK\r\n\r\n" + payload, 1000, /*maxBytes=*/128);
  EXPECT_EQ(res.status, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(res.announcedBodySize, ResponseBodyInfo::kUnknownSize);
  EXPECT_EQ(res.body, payload);
}

// Chunked framing wins over a Content-Length (RFC 9112 §6.3): the size of the body is then unknown.
TEST(ResponseParserStreamingTest, ChunkedBodyWithContentLengthHasNoAnnouncedSize) {
  const StreamedParse res = StreamParse(
      "HTTP/1.1 200 OK\r\ncontent-length: 3\r\ntransfer-encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n", 8);
  EXPECT_EQ(res.status, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(res.announcedBodySize, ResponseBodyInfo::kUnknownSize);
  EXPECT_EQ(res.body, "hello");
}

// Bytes received after the end of a body, streamed or received whole, prevent the reuse of the connection (RFC 9112
// §6.3). The ones not received yet when the response is complete are left to the probe of the idle connection.
TEST(ResponseParserStreamingTest, BytesAfterTheBodyEndPreventReuse) {
  for (const std::string_view response : {
           "HTTP/1.1 200 OK\r\ncontent-length: 5\r\n\r\nhello",
           "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
       }) {
    for (const bool stream : {true, false}) {
      const std::string raw = std::string(response) + "EXTRA";
      for (const std::size_t step : {raw.size(), response.size() - 1, response.size()}) {
        const StreamedParse res = StreamParse(raw, step, kMax, stream);
        EXPECT_EQ(res.status, Http1ResponseParser::Status::Complete) << response << step;
        EXPECT_EQ(res.body, "hello") << response << step;
        EXPECT_EQ(res.keepAlive, step == response.size()) << response << step;  // the last read ends with the response
      }
      const StreamedParse res = StreamParse(response, response.size(), kMax, stream);
      EXPECT_EQ(res.status, Http1ResponseParser::Status::Complete) << response;
      EXPECT_TRUE(res.keepAlive) << response;
    }
  }
}

// So does a Transfer-Encoding whose final coding is not chunked: the body then ends with the connection.
TEST(ResponseParserStreamingTest, NonChunkedTransferEncodingWithContentLengthHasNoAnnouncedSize) {
  const StreamedParse res =
      StreamParse("HTTP/1.1 200 OK\r\ncontent-length: 3\r\ntransfer-encoding: gzip\r\n\r\nhello world", 4);
  EXPECT_EQ(res.status, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(res.announcedBodySize, ResponseBodyInfo::kUnknownSize);
  EXPECT_EQ(res.body, "hello world");
}

// A body cut short is delivered up to its end, then reported as truncated.
TEST(ResponseParserStreamingTest, TruncatedStreamedBodies) {
  for (const std::size_t step : {std::size_t{2}, std::size_t{100}}) {
    const StreamedParse length = StreamParse("HTTP/1.1 200 OK\r\ncontent-length: 10\r\n\r\nabc", step);
    EXPECT_EQ(length.status, Http1ResponseParser::Status::Truncated) << step;
    EXPECT_EQ(length.body, "abc") << step;
    const StreamedParse chunked =
        StreamParse("HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n2\r\nab\r\n5\r\nhel", step);
    EXPECT_EQ(chunked.status, Http1ResponseParser::Status::Truncated) << step;
    EXPECT_EQ(chunked.body, "abhel") << step;
  }
}

namespace {

// Chunked response whose chunks have the given sizes, followed by a trailer. Fills `payload` with its body.
std::string ChunkedWithSizes(std::initializer_list<std::size_t> sizes, std::string& payload) {
  std::string raw = "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n";
  for (const std::size_t size : sizes) {
    const std::string chunk = test::MakePatternedPayload(size);
    payload += chunk;
    char sizeBuf[32];
    const int len = std::snprintf(sizeBuf, sizeof(sizeBuf), "%zx\r\n", size);
    raw.append(sizeBuf, static_cast<std::size_t>(len)).append(chunk).append("\r\n");
  }
  return raw + "0\r\nx-t: 1\r\n\r\n";
}

constexpr std::size_t kDirect = Http1ResponseParser::kMinDirectPieceBytes;

}  // namespace

// Chunks of at least kMinDirectPieceBytes are delivered straight from the receive buffer, one piece each. The smaller
// ones received in a row are gathered and delivered together, before the next large one: the order is kept.
TEST(ResponseParserStreamingTest, SmallChunksAreGatheredLargeOnesAreNot) {
  std::string payload;
  const std::string raw = ChunkedWithSizes({1, 16, kDirect, kDirect - 1, 3 * kDirect, 5, 7}, payload);
  const StreamedParse res = StreamParse(raw, raw.size());
  EXPECT_EQ(res.status, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(res.body, payload);
  const vector<StreamedPiece> expected{
      {.size = 17, .gathered = true},          {.size = kDirect, .gathered = false},
      {.size = kDirect - 1, .gathered = true}, {.size = 3 * kDirect, .gathered = false},
      {.size = 12, .gathered = true},
  };
  EXPECT_EQ(res.pieces, expected);

  // Many small chunks received at once make a single piece.
  std::string smallPayload;
  std::string smallRaw = "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n";
  for (std::size_t idx = 0; idx < 1000U; ++idx) {
    const std::string chunk(1U + (idx % 16U), static_cast<char>('a' + (idx % 26U)));
    smallPayload += chunk;
    char sizeBuf[32];
    const int len = std::snprintf(sizeBuf, sizeof(sizeBuf), "%zx\r\n", chunk.size());
    smallRaw.append(sizeBuf, static_cast<std::size_t>(len)).append(chunk).append("\r\n");
  }
  smallRaw += "0\r\n\r\n";
  const StreamedParse smallStreamParse = StreamParse(smallRaw, smallRaw.size());
  EXPECT_EQ(smallStreamParse.status, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(smallStreamParse.body, smallPayload);
  EXPECT_EQ(smallStreamParse.pieces, (vector<StreamedPiece>{{.size = smallPayload.size(), .gathered = true}}));
}

// The part of a large chunk received in a read is a piece of its own when large enough, gathered otherwise: gathered
// bytes never outlive their read, so a piece never holds more than what the receive buffer held.
TEST(ResponseParserStreamingTest, LargeChunkSplitAcrossReads) {
  std::string payload;
  const std::string raw = ChunkedWithSizes({10 * kDirect, 2}, payload);
  for (const std::size_t step : {std::size_t{100}, kDirect, 4 * kDirect}) {
    const StreamedParse res = StreamParse(raw, step);
    EXPECT_EQ(res.status, Http1ResponseParser::Status::Complete) << step;
    EXPECT_EQ(res.body, payload) << step;
    for (const StreamedPiece& piece : res.pieces) {
      EXPECT_LE(piece.size, res.maxParsedBufferSize) << step;
      if (!piece.gathered) {
        EXPECT_GE(piece.size, kDirect) << step;
      }
    }
  }
  // Received whole, the large chunk is a single piece.
  const StreamedParse whole = StreamParse(raw, raw.size());
  EXPECT_EQ(whole.pieces,
            (vector<StreamedPiece>{{.size = 10 * kDirect, .gathered = false}, {.size = 2, .gathered = true}}));
}

// Gathered chunks are delivered before a failure that follows them.
TEST(ResponseParserStreamingTest, GatheredChunksAreDeliveredBeforeAFailure) {
  const StreamedParse malformed =
      StreamParse("HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\nzz\r\n", 200);
  EXPECT_EQ(malformed.status, Http1ResponseParser::Status::Error);
  EXPECT_EQ(malformed.body, "abcde");
  EXPECT_EQ(malformed.pieces, (vector<StreamedPiece>{{.size = 5, .gathered = true}}));

  const StreamedParse tooLarge = StreamParse(
      "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n3\r\nabc\r\n5;" + std::string(600, 'e'), 1000, 512);
  EXPECT_EQ(tooLarge.status, Http1ResponseParser::Status::TooLarge);
  EXPECT_EQ(tooLarge.body, "abc");
}

// A body following a head that is not streamed is buffered and bounded as usual.
TEST(ResponseParserStreamingTest, HeadCompleteThenBufferedBody) {
  const StreamedParse res =
      StreamParse("HTTP/1.1 302 Found\r\nlocation: /x\r\ncontent-length: 5\r\n\r\nmoved", 3, kMax, /*stream=*/false);
  EXPECT_EQ(res.status, Http1ResponseParser::Status::Complete);
  EXPECT_EQ(res.headCompleteCount, 1);
  EXPECT_EQ(res.body, "moved");

  EXPECT_EQ(StreamParse("HTTP/1.1 200 OK\r\ncontent-length: 100\r\n\r\n" + std::string(100, 'a'), 30, /*maxBytes=*/80,
                        /*stream=*/false)
                .status,
            Http1ResponseParser::Status::TooLarge);
}

// Bodyless responses complete without HeadComplete, even in streaming mode.
TEST(ResponseParserStreamingTest, BodylessResponsesCompleteDirectly) {
  for (const std::string_view raw : {
           std::string_view("HTTP/1.1 204 No Content\r\n\r\n"),
           std::string_view("HTTP/1.1 200 OK\r\ncontent-length: 0\r\n\r\n"),
       }) {
    const StreamedParse res = StreamParse(raw, raw.size());
    EXPECT_EQ(res.status, Http1ResponseParser::Status::Complete) << raw;
    EXPECT_EQ(res.headCompleteCount, 0) << raw;
  }
}

// Chunk metadata never belongs to the body: an endless chunk-size line or trailer line is bounded by maxResponseBytes
// even when the body itself is streamed.
TEST(ResponseParserStreamingTest, EndlessChunkMetadataIsTooLarge) {
  const std::string endlessExtension =
      "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n5;" + std::string(2000, 'e');
  EXPECT_EQ(StreamParse(endlessExtension, 100, /*maxBytes=*/512).status, Http1ResponseParser::Status::TooLarge);
  const std::string endlessTrailer =
      "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n0\r\nx-t: " + std::string(2000, 't');
  EXPECT_EQ(StreamParse(endlessTrailer, 100, /*maxBytes=*/512).status, Http1ResponseParser::Status::TooLarge);
}

// A Content-Encoding'd empty body is installed empty (nothing to decode).
TEST(ResponseParserTest, EmptyBodyWithContentEncodingIsNotDecoded) {
  RawChars bodyBuf;
  RawChars out;
  RawChars tmp;
  DecompressionState state;
  const DecompressionConfig config;
  Http1ResponseParser parser(bodyBuf);
  parser.reset(false);
  parser.setDecodeContext({.pState = &state, .pConfig = &config, .pOut = &out, .pTmp = &tmp});
  HttpResponse resp;
  EXPECT_EQ(
      ParseCopy(parser, "HTTP/1.1 200 OK\r\ncontent-encoding: gzip\r\ncontent-length: 0\r\n\r\n", false, resp, kMax),
      Http1ResponseParser::Status::Complete);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

// Exceeding a decompression limit is told apart from a corrupt body: it is reported as too large.
TEST(ResponseParserDecompress, DecodingBeyondTheLimitsIsTooLarge) {
  const std::string payload = test::MakePatternedPayload(256UL * 1024UL);
  for (const Encoding enc : test::SupportedEncodings()) {
    const RawChars compressed = test::Compress(enc, payload);
    const auto parseWith = [&](auto setLimit) {
      Decode decode;
      decode.config.maxExpansionRatio = 1e9;
      setLimit(decode.config);
      Http1ResponseParser parser(decode.bodyBuf);
      parser.reset(false);
      parser.setDecodeContext(decode.ctx());
      HttpResponse resp;
      return ParseCopy(parser, LengthFramed(GetEncodingStr(enc), std::string_view(compressed)), false, resp,
                       kMaxResponseBytes);
    };
    EXPECT_EQ(parseWith([](DecompressionConfig& cfg) { cfg.maxDecompressedBytes = 64UL * 1024UL; }),
              Http1ResponseParser::Status::TooLarge)
        << GetEncodingStr(enc);
    EXPECT_EQ(parseWith([](DecompressionConfig& cfg) { cfg.maxExpansionRatio = 1.5; }),
              Http1ResponseParser::Status::TooLarge)
        << GetEncodingStr(enc);
    EXPECT_EQ(parseWith([](DecompressionConfig& cfg) { cfg.maxCompressedBytes = 16; }),
              Http1ResponseParser::Status::TooLarge)
        << GetEncodingStr(enc);
    EXPECT_EQ(parseWith([](DecompressionConfig&) {}), Http1ResponseParser::Status::Complete) << GetEncodingStr(enc);
  }
}

// RFC 9112 §9.5: the response bytes received while the request is sent tell whether to stop sending it. The heads of
// the interim responses are skipped, a final status from 300 (or bytes that are no status line) stops it.
TEST(ResponseParserTest, PeekEarlyResponse) {
  using Early = Http1ResponseParser::EarlyResponse;
  struct Case {
    std::string_view received;
    Early expected;
  };
  static constexpr Case kCases[] = {
      {"", Early::None},
      {"HTTP/1.1 41", Early::None},
      {"HTTP/1.1 100 Continue\r\n", Early::None},
      {"HTTP/1.1 103 Early Hints\r\nlink: </a.css>\r\n\r\n", Early::None},
      {"HTTP/1.1 100 Continue\r\n\r\n", Early::Continue},
      {"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 \r\n\r\nHTTP/1.1 2", Early::Continue},
      {"HTTP/1.1 102 Processing\r\n\r\nHTTP/1.1 100 Continue\r\n\r\n", Early::Continue},
      {"HTTP/1.1 200", Early::Success},
      {"HTTP/1.0 204 No Content\r\n\r\n", Early::Success},
      {"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\n", Early::Success},
      {"HTTP/1.1 307 Temporary Redirect\r\n", Early::Rejection},
      {"HTTP/1.1 413 Content Too Large\r\ncontent-length: 0\r\n\r\n", Early::Rejection},
      {"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 500 ", Early::Rejection},
      {"HTTP/1.1 099 Too Low\r\n", Early::Rejection},
      {"HTTP/1.1 2x0 OK\r\n", Early::Rejection},
      {"HTTP/1.1\t200 OK\r\n", Early::Rejection},
      {"garbage, not a response\r\n\r\n", Early::Rejection},
  };
  for (const Case& testCase : kCases) {
    EXPECT_EQ(Http1ResponseParser::PeekEarlyResponse(testCase.received), testCase.expected) << testCase.received;
  }
}

}  // namespace aeronet
