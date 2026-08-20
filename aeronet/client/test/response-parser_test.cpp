#include "../src/response-parser.hpp"

#include <gtest/gtest.h>

#include <cstddef>
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

namespace aeronet {
namespace {

constexpr std::size_t kMax = 1024UL * 1024UL;

// Feed the whole buffer at once (eof=false unless stated). The parser borrows a body-assembly buffer; the
// tests below mirror this by declaring a local `RawChars bodyBuf;` right before each parser.
ResponseParser::Status parseAll(std::string_view raw, HttpResponse& resp, bool head = false, bool eof = false,
                                std::size_t maxBytes = kMax) {
  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(head);
  return parser.parse(raw, eof, resp, maxBytes);
}

}  // namespace

TEST(ResponseParserTest, SimpleContentLength) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", resp);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.reason(), "OK");
  EXPECT_EQ(resp.bodyInMemory(), "hello");
}

TEST(ResponseParserTest, StoresNonReservedHeader) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nLocation: /x\r\n\r\nhi", resp);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.headerValueOrEmpty("location"), "/x");
}

TEST(ResponseParserTest, NoBodyOn204) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 204 No Content\r\n\r\n", resp);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.status(), 204);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST(ResponseParserTest, NoBodyOn304) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 304 Not Modified\r\n\r\n", resp);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.status(), 304);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST(ResponseParserTest, HeadRequestHasNoBodyDespiteContentLength) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n", resp, /*head=*/true);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST(ResponseParserTest, ChunkedDecoding) {
  HttpResponse resp;
  std::string raw =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n"
      "6\r\n world\r\n"
      "0\r\n\r\n";
  auto st = parseAll(raw, resp);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "hello world");
}

TEST(ResponseParserTest, ChunkedWithExtensionAndTrailer) {
  HttpResponse resp;
  std::string raw =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "4;foo=bar\r\nabcd\r\n"
      "0\r\nX-Trailer: v\r\n\r\n";
  auto st = parseAll(raw, resp);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "abcd");
}

TEST(ResponseParserTest, ChunkSizeScannerAcceptsHexOwsAndExtensions) {
  HttpResponse resp;
  const std::string raw =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "\t0A \t;foo=\"bar\tbaz\";flag\r\n"
      "0123456789\r\n"
      "0 ;done\r\n\r\n";
  EXPECT_EQ(parseAll(raw, resp), ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "0123456789");
}

TEST(ResponseParserTest, ChunkMetadataCanArriveOneCrlfByteAtATime) {
  HttpResponse resp;
  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";

  EXPECT_EQ(parser.parse(raw, false, resp, kMax), ResponseParser::Status::NeedMore);
  raw += "1;foo";
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), ResponseParser::Status::NeedMore);
  raw.push_back('\r');
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), ResponseParser::Status::NeedMore);
  raw += "\nx\r";
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), ResponseParser::Status::NeedMore);
  raw += "\n0\r";
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), ResponseParser::Status::NeedMore);
  raw += "\n\r\n";
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "x");
}

TEST(ResponseParserTest, UntilCloseFraming) {
  HttpResponse resp;
  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\n\r\nbody-bytes";
  auto st = parser.parse(raw, /*eof=*/false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::NeedMore);
  st = parser.parse(raw, /*eof=*/true, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "body-bytes");
  EXPECT_FALSE(parser.keepAlive());  // until-close cannot be reused
}

TEST(ResponseParserTest, IncrementalDelivery) {
  HttpResponse resp;
  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string buf;
  std::string_view chunks[] = {"HTTP/1.1 200 OK\r\n", "Content-Length: 11\r\n\r\n", "hello", " world"};
  ResponseParser::Status st = ResponseParser::Status::NeedMore;
  for (auto chunk : chunks) {
    buf.append(chunk);
    st = parser.parse(buf, false, resp, kMax);
  }
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "hello world");
}

TEST(ResponseParserTest, KeepAliveDefaults) {
  RawChars bodyBuf;  // shared: the three parsers below run sequentially

  HttpResponse resp1;
  ResponseParser p1(bodyBuf);
  p1.reset(false);
  p1.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", false, resp1, kMax);
  EXPECT_TRUE(p1.keepAlive());  // HTTP/1.1 default keep-alive

  HttpResponse resp2;
  ResponseParser p2(bodyBuf);
  p2.reset(false);
  p2.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", false, resp2, kMax);
  EXPECT_FALSE(p2.keepAlive());

  HttpResponse resp3;
  ResponseParser p3(bodyBuf);
  p3.reset(false);
  p3.parse("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n", false, resp3, kMax);
  EXPECT_FALSE(p3.keepAlive());  // HTTP/1.0 default close
}

TEST(ResponseParserTest, DiscardsInterim100Continue) {
  HttpResponse resp;
  std::string raw = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
  auto st = parseAll(raw, resp);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "ok");
}

TEST(ResponseParserTest, RejectsGarbageStatusLine) {
  HttpResponse resp;
  auto st = parseAll("NOT-HTTP\r\n\r\n", resp, false, true);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, ConsumedTracksHeadPlusBody) {
  HttpResponse resp;
  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcLEFTOVER";
  auto st = parser.parse(raw, false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "abc");
  EXPECT_EQ(parser.consumed(), raw.size() - std::string_view("LEFTOVER").size());
}

TEST(ResponseParserTest, ContentLengthTruncatedAtEofIsError) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc", resp, /*head=*/false, /*eof=*/true);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, ChunkedIncompleteNeedsMore) {
  HttpResponse resp;
  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel";
  auto st = parser.parse(raw, /*eof=*/false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::NeedMore);
  raw += "lo\r\n0\r\n\r\n";
  st = parser.parse(raw, /*eof=*/false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "hello");
}

TEST(ResponseParserTest, MaxResponseBytesExceededIsError) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n0123456789", resp, /*head=*/false, /*eof=*/false,
                     /*maxBytes=*/5);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, InvalidContentLengthIsError) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n", resp);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, HeadersNeedMoreWhenIncomplete) {
  HttpResponse resp;
  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\nContent-Len";
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), ResponseParser::Status::NeedMore);
  raw += "gth: 0\r";
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), ResponseParser::Status::NeedMore);
  raw += "\n\r";
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), ResponseParser::Status::NeedMore);
  raw.push_back('\n');
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), ResponseParser::Status::Complete);
}

TEST(ResponseParserTest, Http10KeepAliveHeaderEnablesReuse) {
  HttpResponse resp;
  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  auto st = parser.parse("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n", false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_TRUE(parser.keepAlive());  // explicit keep-alive overrides the HTTP/1.0 close default
}

TEST(ResponseParserTest, ConnectionTokenListClosesReuse) {
  // "close" appears as one token of a comma-separated Connection option list (RFC 9110 §7.6.1).
  HttpResponse resp;
  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  auto st = parser.parse("HTTP/1.1 200 OK\r\nConnection: close, foo\r\nContent-Length: 0\r\n\r\n", false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_FALSE(parser.keepAlive());
  // The Connection header is still surfaced losslessly.
  EXPECT_EQ(resp.headerValueOrEmpty("connection"), "close, foo");
}

TEST(ResponseParserTest, Http10ConnectionTokenListEnablesReuse) {
  // "keep-alive" buried in a token list still overrides the HTTP/1.0 close default.
  HttpResponse resp;
  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  auto st = parser.parse("HTTP/1.0 200 OK\r\nConnection: Keep-Alive, Upgrade\r\nContent-Length: 0\r\n\r\n", false, resp,
                         kMax);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_TRUE(parser.keepAlive());
}

TEST(ResponseParserTest, LengthBodyExceedsMaxIsError) {
  HttpResponse resp;
  // Headers fit under maxBytes, but the declared (and delivered) body overflows it.
  std::string raw = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n" + std::string(100, 'a');
  auto st = parseAll(raw, resp, /*head=*/false, /*eof=*/false, /*maxBytes=*/50);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, UntilCloseBodyExceedsMaxIsError) {
  HttpResponse resp;
  std::string raw = "HTTP/1.1 200 OK\r\n\r\n" + std::string(100, 'b');
  auto st = parseAll(raw, resp, /*head=*/false, /*eof=*/false, /*maxBytes=*/50);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, ChunkedBadSizeIsError) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nXYZ\r\ndata", resp);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, ChunkedBodyExceedsMaxIsError) {
  HttpResponse resp;
  std::string raw =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n64\r\n" + std::string(100, 'c') + "\r\n0\r\n\r\n";
  auto st = parseAll(raw, resp, /*head=*/false, /*eof=*/false, /*maxBytes=*/50);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, ChunkedLoweredLimitRejectsAlreadyBufferedBody) {
  HttpResponse resp;
  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  const std::string raw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nx";

  EXPECT_EQ(parser.parse(raw, false, resp, kMax), ResponseParser::Status::NeedMore);
  ASSERT_EQ(bodyBuf.size(), 1);
  EXPECT_EQ(parser.parse(raw, false, resp, 0), ResponseParser::Status::Error);
}

TEST(ResponseParserTest, ChunkedMissingCrlfAtEofIsError) {
  HttpResponse resp;
  auto st =
      parseAll("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello", resp, /*head=*/false, /*eof=*/true);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, ChunkedTrailersTruncatedAtEofIsError) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n", resp, /*head=*/false, /*eof=*/true);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, HeaderLineWithoutNewlineExceedingMaxIsError) {
  HttpResponse resp;
  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  auto st = parser.parse("HTTP/1.1 200 OK\r\nincomplete-no-newline", /*eof=*/false, resp, 10);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

// A still-incomplete header line whose START is within the limit must still be bounded by the *buffered*
// size, otherwise a peer that streams an endless un-terminated header grows the receive buffer without
// limit (the line start never advances, so a `_pos`-only check would never trip).
TEST(ResponseParserTest, IncompleteHeaderBoundedByBufferedSize) {
  HttpResponse resp;
  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  // Status line (17 bytes) fits under maxBytes=50, but the unterminated header pushes the buffer past it.
  const std::string raw = "HTTP/1.1 200 OK\r\nX: " + std::string(100, 'a');  // no trailing CRLF
  auto st = parser.parse(raw, /*eof=*/false, resp, /*maxResponseBytes=*/50);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

// A single oversized but newline-terminated header is rejected (the status-line / empty-line branches used
// to bypass the per-header size check, so such a head could be accepted whole).
TEST(ResponseParserTest, OversizedTerminatedHeaderIsError) {
  HttpResponse resp;
  const std::string raw = "HTTP/1.1 200 OK\r\nX: " + std::string(100, 'a') + "\r\n\r\n";
  auto st = parseAll(raw, resp, /*head=*/false, /*eof=*/false, /*maxBytes=*/50);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, RejectsBadVersionToken) {
  HttpResponse resp;
  auto st = parseAll("HTTPX 200 OK\r\n\r\n", resp, false, true);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, RejectsBadVersionDigits) {
  HttpResponse resp;
  auto st = parseAll("HTTP/X.1 200 OK\r\n\r\n", resp, false, true);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, RejectsBadStatusCode) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 99 OK\r\n\r\n", resp, false, true);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, RejectsHeaderWithoutColon) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nnocolon\r\n\r\n", resp, false, true);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, HeaderLineBeyondMaxBytesIsError) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nA: 1\r\nB: 2\r\n\r\n", resp, /*head=*/false, /*eof=*/false, /*maxBytes=*/20);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, ReparseAfterCompleteStaysComplete) {
  HttpResponse resp;
  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), ResponseParser::Status::Complete);
  // A redundant parse() once Done short-circuits straight back to Complete.
  EXPECT_EQ(parser.parse(raw, false, resp, kMax), ResponseParser::Status::Complete);
}

// "Transfer-Encoding: gzip, chunked" is chunked: only the *last* token decides framing. The chunk bytes
// here are plain text (no codec wired in), so de-chunking yields them verbatim.
TEST(ResponseParserTest, TransferEncodingCommaListLastTokenChunked) {
  HttpResponse resp;
  std::string raw =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n";
  auto st = parseAll(raw, resp);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "hello");
}

// A chunk-size token with a valid hex prefix followed by trailing junk ("5x") is rejected (the parse
// stops before consuming the whole token).
TEST(ResponseParserTest, ChunkedSizeWithTrailingJunkIsError) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5x\r\nhello", resp);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

// A truncated chunk-size line (no terminating LF) that reaches EOF is an error, not an endless wait.
TEST(ResponseParserTest, ChunkedSizeTruncatedAtEofIsError) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3", resp, /*head=*/false, /*eof=*/true);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

// Content-Length with a valid digit prefix then trailing junk ("5x") is rejected.
TEST(ResponseParserTest, ContentLengthWithTrailingJunkIsError) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nContent-Length: 5x\r\n\r\n", resp);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

// A header line whose first character is ':' (empty field name) is malformed.
TEST(ResponseParserTest, RejectsHeaderStartingWithColon) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\n:novalue\r\n\r\n", resp, /*head=*/false, /*eof=*/true);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserTest, ChunkSizeOverflowIsRejectedWithoutRejectingSizeTMax) {
  constexpr std::size_t kMaxHexDigits = sizeof(std::size_t) * 2U;
  const std::string head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";

  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  HttpResponse maxResponse;
  const std::string maxLine = head + std::string(kMaxHexDigits, 'f') + "\r\n";
  EXPECT_EQ(parser.parse(maxLine, false, maxResponse, std::numeric_limits<std::size_t>::max()),
            ResponseParser::Status::NeedMore);

  HttpResponse overflowResponse;
  const std::string overflowLine = head + std::string(kMaxHexDigits + 1U, 'f') + "\r\n";
  EXPECT_EQ(parseAll(overflowLine, overflowResponse), ResponseParser::Status::Error);
}

TEST(ResponseParserTest, ChunkSizeScannerRejectsMalformedSyntax) {
  const auto expectError = [](std::string_view chunkLine) {
    HttpResponse resp;
    std::string raw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    raw.append(chunkLine);
    raw.append("x\r\n0\r\n\r\n");
    EXPECT_EQ(parseAll(raw, resp), ResponseParser::Status::Error) << chunkLine;
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
  for (const std::string_view raw : {
           std::string_view("HTTP/1.1 20x OK\r\n\r\n"),  // trailing junk after digits
           std::string_view("HTTP/1.1 600 X\r\n\r\n"),   // above 599
           std::string_view("HTTP/1.1 abc OK\r\n\r\n"),
       }) {  // non-numeric
    HttpResponse resp;
    EXPECT_EQ(parseAll(raw, resp, /*head=*/false, /*eof=*/true), ResponseParser::Status::Error) << raw;
  }
}

TEST(ResponseParserTest, RejectsBareLfLineEndings) {
  HttpResponse resp;
  EXPECT_EQ(parseAll("HTTP/1.1 200 OK\nContent-Length: 2\n\nhi", resp, false, true), ResponseParser::Status::Error);
}

TEST(ResponseParserTest, RejectsBareLfInChunkedBody) {
  constexpr std::string_view kHead = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
  for (const std::string_view body : {
           std::string_view("5\nhello\r\n0\r\n\r\n"),
           std::string_view("5\r\nhello\n0\r\n\r\n"),
           std::string_view("5\r\nhello\r\n0\n\n"),
           std::string_view("0\r\n\n"),
       }) {
    HttpResponse resp;
    const std::string raw = std::string(kHead) + std::string(body);
    EXPECT_EQ(parseAll(raw, resp, false, true), ResponseParser::Status::Error) << body;
  }
}

// A trailing comma in a Connection option list leaves an empty final token: the scanner must consume the
// list to the end (the loop re-checks its `!value.empty()` guard rather than exiting through the comma).
TEST(ResponseParserTest, ConnectionTrailingCommaClosesReuse) {
  HttpResponse resp;
  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  auto st = parser.parse("HTTP/1.1 200 OK\r\nConnection: close,\r\nContent-Length: 0\r\n\r\n", false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_FALSE(parser.keepAlive());
}

// A chunk-size position holding a bare, empty line (no digits at all) is malformed: the empty token fails
// the hex parse rather than being mistaken for a zero-length final chunk.
TEST(ResponseParserTest, ChunkedEmptySizeLineIsError) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\n", resp);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

// Chunk data cut short (fewer bytes than the announced chunk size) at EOF is an error, not a silent wait.
TEST(ResponseParserTest, ChunkedDataTruncatedAtEofIsError) {
  HttpResponse resp;
  auto st =
      parseAll("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel", resp, /*head=*/false, /*eof=*/true);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

// Chunk data fully delivered but the trailing CRLF not yet arrived (and no EOF): the parser waits for more.
TEST(ResponseParserTest, ChunkedDataCrlfPendingNeedsMore) {
  HttpResponse resp;
  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello";
  auto st = parser.parse(raw, /*eof=*/false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::NeedMore);
  raw += "\r\n0\r\n\r\n";
  st = parser.parse(raw, /*eof=*/false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "hello");
}

TEST(ResponseParserTest, ChunkedBodyTransfersAllocationAndPreservesScratchCapacity) {
  const std::string payload(4096, 'x');
  std::string raw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1000\r\n";
  raw.append(payload);

  RawChars bodyBuf;
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  HttpResponse first;
  ASSERT_EQ(parser.parse(raw, false, first, kMax), ResponseParser::Status::NeedMore);
  const char* firstAllocation = bodyBuf.data();
  const std::size_t scratchCapacity = bodyBuf.capacity();
  ASSERT_NE(firstAllocation, nullptr);
  ASSERT_EQ(bodyBuf.size(), payload.size());

  raw.append("\r\n0\r\n\r\n");
  ASSERT_EQ(parser.parse(raw, false, first, kMax), ResponseParser::Status::Complete);
  EXPECT_TRUE(first.hasBodyCaptured());
  EXPECT_EQ(first.bodyInMemory(), payload);
  EXPECT_EQ(first.bodyInMemory().data(), firstAllocation);
  EXPECT_TRUE(bodyBuf.empty());
  EXPECT_EQ(bodyBuf.capacity(), scratchCapacity);
  EXPECT_NE(bodyBuf.data(), firstAllocation);

  // The replacement is used directly by the next exchange, while the first response remains independent.
  const std::string secondPayload(2048, 'y');
  std::string secondRaw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n800\r\n";
  secondRaw.append(secondPayload);
  secondRaw.append("\r\n0\r\n\r\n");
  const char* secondAllocation = bodyBuf.data();
  parser.reset(false);
  HttpResponse second;
  ASSERT_EQ(parser.parse(secondRaw, false, second, kMax), ResponseParser::Status::Complete);
  EXPECT_EQ(second.bodyInMemory(), secondPayload);
  EXPECT_EQ(second.bodyInMemory().data(), secondAllocation);
  EXPECT_EQ(first.bodyInMemory(), payload);
}

TEST(ResponseParserTest, ChunkDataDelimiterIsValidated) {
  constexpr std::string_view kHead = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
  for (const std::string_view body : {std::string_view("1\r\nxX\n0\r\n\r\n"), std::string_view("1\r\nx\rX0\r\n\r\n")}) {
    HttpResponse resp;
    const std::string raw = std::string(kHead) + std::string(body);
    EXPECT_EQ(parseAll(raw, resp, false, true), ResponseParser::Status::Error) << body;
  }

  HttpResponse truncated;
  const std::string trailingCr = std::string(kHead) + "1\r\nx\r";
  EXPECT_EQ(parseAll(trailingCr, truncated, false, true), ResponseParser::Status::Error);
}

TEST(ResponseParserTest, OversizedScratchStaysReusableForSmallChunkedBody) {
  RawChars bodyBuf(4096);
  const char* scratchAllocation = bodyBuf.data();
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  HttpResponse resp;
  ASSERT_EQ(
      parser.parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n", false, resp, kMax),
      ResponseParser::Status::Complete);
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
  ResponseParser parser(bodyBuf);
  parser.reset(false);
  std::string raw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n";
  auto st = parser.parse(raw, /*eof=*/false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::NeedMore);
  raw += "\r\n";  // close the (empty) trailer block
  st = parser.parse(raw, /*eof=*/false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

// A status line with no reason phrase ("HTTP/1.1 200") is accepted: the code is parsed and the (absent)
// reason is simply left empty.
TEST(ResponseParserTest, StatusLineWithoutReasonPhrase) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n", resp);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.reason().empty());
}

namespace {

constexpr std::size_t kMaxResponseBytes = 8UL * 1024UL * 1024UL;

// Reusable decode plumbing for one parse() call.
struct Decode {
  internal::DecompressionState state;
  DecompressionConfig config;
  RawChars out;
  RawChars tmp;
  RawChars bodyBuf;  // borrowed by the ResponseParser for chunked reassembly

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
  const std::string payload = test::MakePatternedPayload(64UL * 1024UL);
  for (const Encoding enc : test::SupportedEncodings()) {
    const RawChars compressed = test::Compress(enc, payload);
    Decode decode;
    const std::size_t decodeScratchCapacity = payload.size();
    decode.tmp = RawChars(decodeScratchCapacity);
    const char* decodedAllocation = decode.tmp.data();
    ResponseParser parser(decode.bodyBuf);
    parser.reset(false);
    parser.setDecodeContext(decode.ctx());
    HttpResponse resp;
    const std::string raw = LengthFramed(GetEncodingStr(enc), std::string_view(compressed));
    const auto st = parser.parse(raw, false, resp, kMaxResponseBytes);
    EXPECT_EQ(st, ResponseParser::Status::Complete) << GetEncodingStr(enc);
    EXPECT_EQ(resp.bodyInMemory(), payload) << GetEncodingStr(enc);
    EXPECT_TRUE(resp.hasBodyCaptured()) << GetEncodingStr(enc);
    EXPECT_EQ(resp.bodyInMemory().data(), decodedAllocation) << GetEncodingStr(enc);
    EXPECT_EQ(decode.out.capacity(), decodeScratchCapacity) << GetEncodingStr(enc);
    EXPECT_NE(decode.out.data(), decodedAllocation) << GetEncodingStr(enc);
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
  ResponseParser parser(decode.bodyBuf);
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const auto st = parser.parse(raw, false, resp, kMaxResponseBytes);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), payload);
  EXPECT_TRUE(resp.headerValueOrEmpty("content-encoding").empty());
}

TEST(ResponseParserDecompress, NoContentEncodingPassesThrough) {
  Decode decode;
  ResponseParser parser(decode.bodyBuf);
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const auto st = parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", false, resp, kMaxResponseBytes);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "hello");
}

TEST(ResponseParserDecompress, IdentityEncodingPassesThroughAndDropsHeader) {
  Decode decode;
  ResponseParser parser(decode.bodyBuf);
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const auto st = parser.parse(LengthFramed("identity", "hello"), false, resp, kMaxResponseBytes);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "hello");
  EXPECT_TRUE(resp.headerValueOrEmpty("content-encoding").empty());
}

TEST(ResponseParserDecompress, IdentityChunkedTransfersReassemblyAllocation) {
  Decode decode;
  decode.bodyBuf = RawChars(8);
  const char* bodyAllocation = decode.bodyBuf.data();
  ResponseParser parser(decode.bodyBuf);
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const auto st = parser.parse(
      "HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
      false, resp, kMaxResponseBytes);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_TRUE(resp.hasBodyCaptured());
  EXPECT_EQ(resp.bodyInMemory(), "hello");
  EXPECT_EQ(resp.bodyInMemory().data(), bodyAllocation);
  EXPECT_EQ(decode.bodyBuf.capacity(), 8);
  EXPECT_NE(decode.bodyBuf.data(), bodyAllocation);
  EXPECT_TRUE(resp.headerValueOrEmpty("content-encoding").empty());
}

TEST(ResponseParserDecompress, UnsupportedEncodingIsError) {
  Decode decode;
  ResponseParser parser(decode.bodyBuf);
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const auto st = parser.parse(LengthFramed("made-up", "whatever-bytes"), false, resp, kMaxResponseBytes);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

TEST(ResponseParserDecompress, GarbageCompressedBodyIsError) {
  if (test::SupportedEncodings().empty()) {
    GTEST_SKIP() << "no codec compiled in";
  }
  const Encoding enc = test::SupportedEncodings().front();
  Decode decode;
  ResponseParser parser(decode.bodyBuf);
  parser.reset(false);
  parser.setDecodeContext(decode.ctx());
  HttpResponse resp;
  const auto st = parser.parse(LengthFramed(GetEncodingStr(enc), "not-a-valid-frame"), false, resp, kMaxResponseBytes);
  EXPECT_EQ(st, ResponseParser::Status::Error);
}

}  // namespace aeronet
