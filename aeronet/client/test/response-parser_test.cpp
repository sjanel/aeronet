#include "response-parser.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "aeronet/http-response.hpp"

namespace aeronet {
namespace {

constexpr std::size_t kMax = 1024UL * 1024UL;

// Feed the whole buffer at once (eof=false unless stated).
ResponseParser::Status parseAll(std::string_view raw, HttpResponse& resp, bool head = false, bool eof = false,
                                std::size_t maxBytes = kMax) {
  ResponseParser parser;
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

TEST(ResponseParserTest, UntilCloseFraming) {
  HttpResponse resp;
  ResponseParser parser;
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
  ResponseParser parser;
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
  HttpResponse resp1;
  ResponseParser p1;
  p1.reset(false);
  p1.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", false, resp1, kMax);
  EXPECT_TRUE(p1.keepAlive());  // HTTP/1.1 default keep-alive

  HttpResponse resp2;
  ResponseParser p2;
  p2.reset(false);
  p2.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", false, resp2, kMax);
  EXPECT_FALSE(p2.keepAlive());

  HttpResponse resp3;
  ResponseParser p3;
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
  ResponseParser parser;
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
  ResponseParser parser;
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
  ResponseParser parser;
  parser.reset(false);
  auto st = parser.parse("HTTP/1.1 200 OK\r\nContent-Len", /*eof=*/false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::NeedMore);
}

TEST(ResponseParserTest, Http10KeepAliveHeaderEnablesReuse) {
  HttpResponse resp;
  ResponseParser parser;
  parser.reset(false);
  auto st = parser.parse("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n", false, resp, kMax);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_TRUE(parser.keepAlive());  // explicit keep-alive overrides the HTTP/1.0 close default
}

TEST(ResponseParserTest, ConnectionTokenListClosesReuse) {
  // "close" appears as one token of a comma-separated Connection option list (RFC 9110 §7.6.1).
  HttpResponse resp;
  ResponseParser parser;
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
  ResponseParser parser;
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
  ResponseParser parser;
  parser.reset(false);
  auto st = parser.parse("HTTP/1.1 200 OK\r\nincomplete-no-newline", /*eof=*/false, resp, 10);
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
  ResponseParser parser;
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

// Status codes that parse to a value but leave trailing junk, or fall outside the valid 100..599 range,
// are all rejected.
TEST(ResponseParserTest, RejectsStatusCodeTrailingJunkOrOutOfRange) {
  for (const std::string_view raw : {std::string_view("HTTP/1.1 20x OK\r\n\r\n"),     // trailing junk after digits
                                     std::string_view("HTTP/1.1 600 X\r\n\r\n"),      // above 599
                                     std::string_view("HTTP/1.1 abc OK\r\n\r\n")}) {  // non-numeric
    HttpResponse resp;
    EXPECT_EQ(parseAll(raw, resp, /*head=*/false, /*eof=*/true), ResponseParser::Status::Error) << raw;
  }
}

// Bare-LF line endings (no CR) are tolerated in the status line and headers.
TEST(ResponseParserTest, AcceptsBareLfLineEndings) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\nContent-Length: 2\n\nhi", resp);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "hi");
}

// Bare-LF line endings are likewise tolerated inside a chunked body (chunk-size and chunk-data CRLFs).
TEST(ResponseParserTest, AcceptsBareLfInChunkedBody) {
  HttpResponse resp;
  auto st = parseAll("HTTP/1.1 200 OK\nTransfer-Encoding: chunked\n\n5\nhello\n0\n\n", resp);
  EXPECT_EQ(st, ResponseParser::Status::Complete);
  EXPECT_EQ(resp.bodyInMemory(), "hello");
}

}  // namespace aeronet
