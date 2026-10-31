#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <thread>

#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;

namespace aeronet {

namespace {

// Shared server used by most tests. A short poll interval keeps NeedMore round-trips fast.
test::TestServer ts;
const auto port = ts.port();

// Echo the request body back in the response.
void InstallEchoHandler() {
  ts.router().setDefault([](const HttpRequestView& req) { return req.makeResponse(req.body()); });
}

// Sends an HTTP request with a fixed-length body split into two TCP writes.
// Returns the raw response string.
std::string SendSplit(uint16_t targetPort, std::string_view head, std::string_view firstChunk,
                      std::string_view restChunk, std::chrono::milliseconds delayBetween = 15ms) {
  test::ClientConnection conn(targetPort);
  NativeHandle fd = conn.fd();
  test::sendAll(fd, head);
  test::sendAll(fd, firstChunk);
  std::this_thread::sleep_for(delayBetween);
  test::sendAll(fd, restChunk);
  return test::recvUntilClosed(fd);
}

// Sends head, then body in parts of at most partSize bytes, each in its own write, pausing between them so that the
// server reads them separately. Returns the raw response string.
std::string SendInParts(uint16_t targetPort, std::string_view head, std::string_view body, std::size_t partSize,
                        std::chrono::microseconds delayBetween = 2ms) {
  test::ClientConnection conn(targetPort);
  NativeHandle fd = conn.fd();
  EXPECT_TRUE(SetTcpNoDelay(fd));  // the parts are not coalesced while the previous one is not acknowledged
  test::sendAll(fd, head);
  while (!body.empty()) {
    const std::size_t partLen = std::min(partSize, body.size());
    test::sendAll(fd, body.substr(0, partLen));
    body.remove_prefix(partLen);
    std::this_thread::sleep_for(delayBetween);
  }
  return test::recvUntilClosed(fd);
}

// The response to 'raw', waited for at most 2 s: a server waiting for the rest of a request that can no longer be valid
// (until a timeout, if configured) answers nothing.
std::string SendAndCollectPromptly(std::string_view raw) {
  test::ClientConnection conn(port);
  test::sendAll(conn.fd(), raw);
  return test::recvUntilClosed(conn.fd(), 2s);
}

constexpr std::string_view kChunkedHead =
    "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";

// FNV-1a: tells large bodies apart without echoing them.
uint64_t Fnv1a(std::string_view data) {
  uint64_t hash = 14695981039346656037ULL;
  for (const char ch : data) {
    hash = (hash ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
  }
  return hash;
}

}  // namespace

TEST(HttpParserRequestHead, BareCRIsRejectedWithoutWaitingForAnotherLineTerminator) {
  const std::string resp = test::sendAndCollect(port, "GET / HTTP/1.1\r\nHost: bad\rX");
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
}

// A client using LF as line terminator never sends the CR the head is searched for: the request is answered 400 as soon
// as it is received, instead of waiting for the rest of a head that cannot be valid.
TEST(HttpParserRequestHead, BareLfLineEndingsAreRejectedWithoutWaiting) {
  InstallEchoHandler();
  for (const std::string_view raw : {
           "GET / HTTP/1.1\nHost: x\n\n",
           "GET / HTTP/1.1\nHost: x\nConnection: close\n\n",
           "GET / HTTP/1.0\n\n",
           "GET / HTTP/1.1\nHost: x\n",
           "POST / HTTP/1.1\nHost: x\nContent-Length: 5\n\nHello",
           "GET / HTTP/1.1\r\nHost: x\n\n",
           "GET / HTTP/1.1\r\nHost: x\r\n\n",
           "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\n",
           "GET / HTTP/1.1\nHost: x\r\n\r\n",
       }) {
    SCOPED_TRACE(raw);
    const std::string resp = SendAndCollectPromptly(raw);
    ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
    EXPECT_EQ(test::countOccurrences(resp, "HTTP/1.1 "), 1) << resp;
  }
}

// The line being received, not the ones parsed by the previous reads, is the one a bare LF is looked for in.
TEST(HttpParserRequestHead, BareLfInTheRestOfAHeadReceivedInSeveralReadsIsRejected) {
  InstallEchoHandler();
  for (const std::string_view rest : {"\nHost: x\n\n", "Host: x\n\n", "\n", "st: x\r\nX: y\n"}) {
    SCOPED_TRACE(rest);
    test::ClientConnection conn(port);
    ASSERT_TRUE(SetTcpNoDelay(conn.fd()));
    test::sendAll(conn.fd(), "GET / HTTP/1.1\r\nHo");
    std::this_thread::sleep_for(15ms);
    test::sendAll(conn.fd(), rest);
    const std::string resp = test::recvUntilClosed(conn.fd(), 2s);
    ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
  }
}

// A CR followed by its LF in another read is a CRLF, whichever line it ends.
TEST(HttpParserRequestHead, CrLfSplitBetweenReadsIsNotABareLf) {
  InstallEchoHandler();
  struct Split {
    std::string_view first;
    std::string_view rest;
  };
  for (const auto& [first, rest] : {
           Split{"GET / HTTP/1.1\r", "\nHost: x\r\nConnection: close\r\n\r\n"},
           Split{"GET / HTTP/1.1\r\nHost: x\r", "\nConnection: close\r\n\r\n"},
           Split{"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r", "\n"},
       }) {
    SCOPED_TRACE(first);
    const std::string resp = SendSplit(port, first, "", rest);
    ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  }
}

TEST(HttpParserRequestHead, WaitsWhenHeaderCRIsLastBufferedByte) {
  InstallEchoHandler();
  const std::string resp = SendSplit(port, "GET / HTTP/1.1\r\nHost: x\r", {}, "\nConnection: close\r\n\r\n");
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// =============================================================================
// Fixed-length body (decodeFixedLengthBody)
// =============================================================================

// GET with no Content-Length: body treated as empty immediately (no waiting).
TEST(HttpParserFixedLength, NoContentLength) {
  InstallEchoHandler();
  std::string req = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// Content-Length: 0 — empty body, Ready immediately.
TEST(HttpParserFixedLength, ZeroContentLength) {
  InstallEchoHandler();
  std::string req = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// Body fully present in the first TCP segment — Ready path.
TEST(HttpParserFixedLength, FullBodyInOneShot) {
  InstallEchoHandler();
  std::string body = "Hello";
  std::string req = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nConnection: close\r\n\r\n" + body;
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("Hello")) << resp;
}

// NeedMore path (line 132): headers arrive first, body bytes arrive later.
// The server must buffer partial data and only dispatch once totalNeeded bytes are present.
TEST(HttpParserFixedLength, NeedMore_PartialBodyThenRest) {
  InstallEchoHandler();
  std::string head = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\nConnection: close\r\n\r\n";
  std::string resp = SendSplit(port, head, "Hello", "World");
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("HelloWorld")) << resp;
}

// NeedMore with only 1 byte of body delivered initially.
TEST(HttpParserFixedLength, NeedMore_OneByteAtATime) {
  InstallEchoHandler();
  std::string head = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nConnection: close\r\n\r\n";
  std::string resp = SendSplit(port, head, "A", "BCD");
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("ABCD")) << resp;
}

// Content-Length with non-numeric value → 400 Bad Request.
TEST(HttpParserFixedLength, InvalidContentLength_NonNumeric) {
  std::string req = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\nConnection: close\r\n\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
}

// Content-Length with trailing non-numeric chars → 400 Bad Request.
TEST(HttpParserFixedLength, InvalidContentLength_TrailingGarbage) {
  std::string req = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5abc\r\nConnection: close\r\n\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
}

// Content-Length value exceeds configured maxBodyBytes → 413 Payload Too Large.
TEST(HttpParserFixedLength, ContentLengthExceedsMaxBodyBytes) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxBodyBytes(16); });
  ts.resetRouterAndGet().setDefault([](const HttpRequestView& req) { return req.makeResponse(req.body()); });

  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 32\r\nConnection: close\r\n\r\n" + std::string(32, 'X');
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 413")) << resp;
}

// Expect: 100-continue with Content-Length: 0 — server must NOT send 100 Continue.
TEST(HttpParserFixedLength, Expect100Continue_ZeroBody_NoInterim) {
  InstallEchoHandler();
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_FALSE(resp.starts_with("HTTP/1.1 100")) << "Should not receive 100 Continue for zero-length body\n" << resp;
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// Expect: 100-continue with a non-zero body — server sends 100 Continue, then the final 200.
TEST(HttpParserFixedLength, Expect100Continue_NonZeroBody_InterimThenFinal) {
  InstallEchoHandler();
  test::ClientConnection conn(port);
  NativeHandle fd = conn.fd();
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: close\r\n\r\nHello";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 100 Continue")) << resp;
  ASSERT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
}

// =============================================================================
// Chunked body (decodeChunkedBody)
// =============================================================================

// Minimal chunked request: a single chunk followed by the terminator.
TEST(HttpParserChunked, SingleChunk) {
  InstallEchoHandler();
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "5\r\nHello\r\n"
      "0\r\n\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("Hello")) << resp;
}

// Multiple chunks must be concatenated in order.
TEST(HttpParserChunked, MultipleChunks) {
  InstallEchoHandler();
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "3\r\nfoo\r\n"
      "3\r\nbar\r\n"
      "3\r\nbaz\r\n"
      "0\r\n\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("foobarbaz")) << resp;
}

// Just the terminator chunk: body must be empty.
TEST(HttpParserChunked, EmptyBody_OnlyTerminator) {
  InstallEchoHandler();
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "0\r\n\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// NeedMore path (line 151): the chunk-size line arrives without its terminating CRLF.
// The server must wait for more data before it can parse the chunk size.
TEST(HttpParserChunked, NeedMore_NoChunkSizeCRLF) {
  InstallEchoHandler();
  std::string head = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  // Send the chunk size "5" without its trailing CRLF.
  std::string resp = SendSplit(port, head, "5", "\r\nHello\r\n0\r\n\r\n");
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("Hello")) << resp;
}

TEST(HttpParserChunked, BareCarriageReturnInChunkSizeLineIsRejected) {
  const std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "5\rX";

  const std::string resp = test::sendAndCollect(port, req);

  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
}

// A bare LF never completes into the CRLF ending a line of the chunked framing: a chunk-size line, the line ending
// chunk data, the empty line ending the body, or a trailer line. Rejected as soon as received, with no CR following it
// for the framing to find.
TEST(HttpParserChunked, BareLfLineEndingsAreRejectedWithoutWaiting) {
  InstallEchoHandler();
  for (const std::string_view chunks : {
           "5\nhello\n0\n\n",                        // LF-only framing
           "5\n",                                    // first chunk-size line
           "5;ext\n",                                // ... with an extension
           "5\r\nhello\r\n5\nhello\n",               // a following chunk-size line
           "5\r\nhello\n",                           // end of the chunk data
           "5\r\nhello\r\n0\r\n\n",                  // empty line ending the body
           "0\r\n\n",                                // ... of a body without chunk
           "5\r\nhello\r\n0\r\nX-Tag: v\n\n",        // trailer line
           "5\r\nhello\r\n0\r\nX-A: 1\r\nX-B: 2\n",  // second trailer line
       }) {
    SCOPED_TRACE(chunks);
    const std::string resp = SendAndCollectPromptly(std::string(kChunkedHead).append(chunks));
    ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
    EXPECT_EQ(test::countOccurrences(resp, "HTTP/1.1 "), 1) << resp;
  }
}

// ... whether it is received in the first read of the framing or in a later one.
TEST(HttpParserChunked, BareLfInChunkedFramingReceivedInSeveralReadsIsRejected) {
  InstallEchoHandler();
  struct Split {
    std::string_view first;
    std::string_view rest;
  };
  for (const auto& [first, rest] : {
           Split{"5", "\n"},
           Split{"5\r\nhel", "lo\n"},
           Split{"5\r\nhello", "\n"},
           Split{"5\r\nhello\r\n", "5\nhello\n"},
           Split{"5\r\nhello\r\n0\r\n", "\n"},
           Split{"5\r\nhello\r\n0\r\nX-A: ", "1\n\n"},
           Split{"5\r\nhello\r\n0\r\nX-A: 1\r\nX-B", ": 2\n"},
       }) {
    SCOPED_TRACE(std::string(first).append(" | ").append(rest));
    const std::string resp = SendSplit(port, std::string(kChunkedHead).append(first), "", rest);
    ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
  }
}

// The CR and the LF of a CRLF of the chunked framing received in two reads are not a bare LF.
TEST(HttpParserChunked, CrLfSplitBetweenReadsIsNotABareLf) {
  InstallEchoHandler();
  struct Split {
    std::string_view first;
    std::string_view rest;
  };
  for (const auto& [first, rest] : {
           Split{"5\r", "\nhello\r\n0\r\n\r\n"},            // chunk-size line
           Split{"5\r\nhello\r", "\n0\r\n\r\n"},            // end of the chunk data
           Split{"5\r\nhello\r\n0\r\n\r", "\n"},            // empty line ending the body
           Split{"5\r\nhello\r\n0\r\nX-A: 1\r", "\n\r\n"},  // trailer line
           Split{"5\r\nhello\r\n0\r\nX-A: 1\r\n\r", "\n"},  // empty line ending the trailers
       }) {
    SCOPED_TRACE(first);
    const std::string resp = SendSplit(port, std::string(kChunkedHead).append(first), "", rest);
    ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
    EXPECT_TRUE(resp.contains("hello")) << resp;
  }
}

// NeedMore path (line 132): chunk-size line is complete but chunk data is incomplete.
// The server knows how many bytes to expect but has not received them all yet.
TEST(HttpParserChunked, NeedMore_PartialChunkData) {
  InstallEchoHandler();
  std::string head =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "5\r\nHel";  // Only 3 of 5 data bytes
  std::string resp = SendSplit(port, head, "", "lo\r\n0\r\n\r\n");
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("Hello")) << resp;
}

// NeedMore after receiving "0\r\n" but without the final "\r\n" terminator.
TEST(HttpParserChunked, NeedMore_IncompleteTerminator) {
  InstallEchoHandler();
  std::string head =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "5\r\nHello\r\n"
      "0\r\n";  // Missing the final \r\n
  std::string resp = SendSplit(port, head, "", "\r\n");
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// Chunk with a semicolon extension (RFC 7230 §4.1.1): extension must be silently ignored.
TEST(HttpParserChunked, ChunkExtension_Ignored) {
  InstallEchoHandler();
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "5;name=value\r\nHello\r\n"
      "0\r\n\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("Hello")) << resp;
}

// Chunk extensions may contain obs-text (in a quoted-string, RFC 9112 §7.1.1), but no control character.
TEST(HttpParserChunked, ChunkExtensionObsTextIsAcceptedControlCharactersAreNot) {
  InstallEchoHandler();
  std::string resp = test::sendAndCollect(port,
                                          "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: "
                                          "close\r\n\r\n"
                                          "5;name=\"caf\xC3\xA9\"\r\nHello\r\n"
                                          "0\r\n\r\n");
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.ends_with("Hello")) << resp;

  resp = test::sendAndCollect(port,
                              "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                              "5;name=\"caf\xC3\xA9\x7F\"\r\nHello\r\n"
                              "0\r\n\r\n");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
}

// Non-hexadecimal chunk size → 400 Bad Request.
TEST(HttpParserChunked, InvalidChunkSize_NonHex) {
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "XY\r\nbad\r\n"
      "0\r\n\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
}

// Chunk size alone (without data bytes) exceeds maxBodyBytes → 413 Payload Too Large.
TEST(HttpParserChunked, ChunkSizeExceedsMaxBodyBytes) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) { cfg.withMaxBodyBytes(8U); });
  ts.router().setDefault([](const HttpRequestView& req) { return req.makeResponse(req.body()); });
  // Declare a chunk of 32 bytes which exceeds the 8-byte limit.
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "20\r\n" +
      std::string(32, 'A') + "\r\n0\r\n\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.contains("413")) << resp;
}

// Missing CRLF after chunk data → 400 Bad Request.
TEST(HttpParserChunked, MissingCRLFAfterChunkData) {
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "5\r\nHelloXX";  // "XX" instead of "\r\n"
  // Use sendAndCollect which also sends a subsequent request to flush the connection.
  test::ClientConnection conn(port);
  NativeHandle fd = conn.fd();
  test::sendAll(fd, req);
  // Force the server to see the bad terminator by completing to something it can parse.
  test::sendAll(fd, "\r\n0\r\n\r\n");
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("400")) << resp;
}

// Chunked request with Expect: 100-continue → server sends interim 100 then processes body.
TEST(HttpParserChunked, Expect100Continue) {
  InstallEchoHandler();
  test::ClientConnection conn(port);
  NativeHandle fd = conn.fd();
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\nConnection: "
      "close\r\n\r\n"
      "5\r\nHello\r\n"
      "0\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 100 Continue")) << resp;
  ASSERT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
}

// =============================================================================
// Chunked body with trailers
// =============================================================================

// A valid custom trailer header must be accessible via req.trailers().
TEST(HttpParserChunkedTrailers, ValidTrailer) {
  ts.router().setDefault([](const HttpRequestView& req) {
    auto val = req.trailerValueOrEmpty("x-checksum");
    return req.makeResponse(val);
  });
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "4\r\ndata\r\n"
      "0\r\n"
      "X-Checksum: abc123\r\n"
      "\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("abc123")) << resp;
}

TEST(HttpParserChunkedTrailers, ParsedTrailerValuesShouldBeTrimmed) {
  ts.router().setDefault([](const HttpRequestView& req) {
    auto val = req.trailerValueOrEmpty("x-checksum");
    return req.makeResponse(val);
  });
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "4\r\ndata\r\n"
      "0\r\n"
      "X-Checksum: \t abc123  \r\n"
      "\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200 ")) << resp;
  EXPECT_TRUE(resp.ends_with("abc123")) << resp;
}

// Trailer values may contain obs-text (bytes 0x80-0xFF), kept as is (RFC 9110 §5.5).
TEST(HttpParserChunkedTrailers, ObsTextTrailerValueIsKeptAsIs) {
  ts.router().setDefault(
      [](const HttpRequestView& req) { return req.makeResponse(req.trailerValueOrEmpty("x-name")); });
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "4\r\ndata\r\n"
      "0\r\n"
      "X-Name: r\xC3\xA9sum\xC3\xA9 \xE9\r\n"
      "\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200 ")) << resp;
  EXPECT_TRUE(resp.ends_with("\r\n\r\nr\xC3\xA9sum\xC3\xA9 \xE9")) << resp;
}

// NeedMore in trailer parsing: the trailer line arrives without its CRLF.
TEST(HttpParserChunkedTrailers, NeedMore_PartialTrailerLine) {
  ts.router().setDefault([](const HttpRequestView& req) {
    auto val = req.trailerValueOrEmpty("x-tag");
    return req.makeResponse(val);
  });
  std::string head =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "3\r\nfoo\r\n"
      "0\r\n"
      "X-Tag: hel";  // incomplete trailer value
  std::string resp = SendSplit(port, head, "", "lo\r\n\r\n");
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("hello")) << resp;
}

TEST(HttpParserChunkedTrailers, InvalidTrailerHeaderName) {
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "3\r\nfoo\r\n"
      "0\r\n"
      "x-trailer : test\r\n"
      "\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
}

TEST(HttpParserChunkedTrailers, InvalidTrailerCR) {
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "3\r\nfoo\r\n"
      "0\r\n"
      "x-traile\r: test\r\n"
      "\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
}

TEST(HttpParserChunkedTrailers, InvalidTrailerHeaderValue) {
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "3\r\nfoo\r\n"
      "0\r\n"
      "x-trailer: ";
  req.push_back('\x01');
  req.append("\r\n\r\n");
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
}

// A forbidden header used as a trailer → 400 Bad Request.
TEST(HttpParserChunkedTrailers, ForbiddenTrailerHeader) {
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "3\r\nfoo\r\n"
      "0\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
}

// Trailer line without a colon separator → 400 Bad Request.
TEST(HttpParserChunkedTrailers, MalformedTrailer_NoColon) {
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "3\r\nfoo\r\n"
      "0\r\n"
      "X-Bad-Trailer-No-Colon\r\n"
      "\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.contains("400")) << resp;
}

// Trailer block exceeds maxHeaderBytes → 431 Request Header Fields Too Large.
TEST(HttpParserChunkedTrailers, TrailerTooLarge) {
  // 128 is the minimum accepted value for maxHeaderBytes.
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) { cfg.withMaxHeaderBytes(128); });
  ts.router().setDefault([](const HttpRequestView& req) { return req.makeResponse(req.body()); });
  // Build a trailer value long enough to exceed the 128-byte limit.
  std::string longValue(256, 'V');
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "3\r\nfoo\r\n"
      "0\r\n"
      "X-Big: " +
      longValue + "\r\n\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 431")) << resp;
}

// =============================================================================
// Large body — ensure chunk processing with many segments works correctly.
// =============================================================================

TEST(HttpParserChunked, LargeBody_ManyChunks) {
  InstallEchoHandler();
  constexpr std::size_t kChunkDataSize = 512;
  constexpr int kChunkCount = 64;

  ts.resetConfig();

  std::string req = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  std::string expected;
  char hex[16];
  for (int i = 0; i < kChunkCount; ++i) {
    std::string data(kChunkDataSize, static_cast<char>('A' + (i % 26)));
    req.append(std::string_view(hex, ToLowerHex(kChunkDataSize, hex)));
    req += "\r\n" + data + "\r\n";
    expected += data;
  }
  req += "0\r\n\r\n";

  test::ClientConnection conn(port);
  NativeHandle fd = conn.fd();
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  // Verify the first and last chunk segments are present in the body portion of the response.
  ASSERT_TRUE(resp.contains(expected.substr(0, 8))) << "First chunk data missing";
  ASSERT_TRUE(resp.contains(expected.substr(expected.size() - 8))) << "Last chunk data missing";
}

// Large fixed-length body to verify buffering across chunk boundaries.
TEST(HttpParserFixedLength, LargeBody) {
  InstallEchoHandler();
  constexpr std::size_t kBodySize = 128UL * 1024;  // 128 KiB
  std::string body(kBodySize, 'Z');
  std::string req = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(kBodySize) +
                    "\r\nConnection: close\r\n\r\n" + body;
  test::ClientConnection conn(port);
  NativeHandle fd = conn.fd();

  ts.resetConfig();

  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// =============================================================================
// Chunked body received in several reads: decoded once, from where the previous read stopped.
// =============================================================================

// Whatever the byte where the reads of a body end (in a chunk size, its extension, its data, the CRLF after it, the
// last chunk or the trailers), the decoded body and trailers are the same.
TEST(HttpParserChunkedResume, BodyAndTrailersReceivedByteByByte) {
  ts.resetConfig();
  ts.router().setDefault([](const HttpRequestView& req) {
    return req.makeResponse(
        std::format("{}|{}|{}", req.body(), req.trailerValueOrEmpty("x-sum"), req.trailerValueOrEmpty("x-note")));
  });
  static constexpr std::string_view kBody =
      "3\r\nfoo\r\n"
      "6;ext=1\r\nbarbaz\r\n"
      "a\r\n0123456789\r\n"
      "0\r\n"
      "X-Sum: 42\r\n"
      "X-Note: n\r\n"
      "\r\n";
  const std::string resp = SendInParts(port, kChunkedHead, kBody, 1);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.ends_with("foobarbaz0123456789|42|n")) << resp;
}

TEST(HttpParserChunkedResume, BodyWithoutTrailersReceivedByteByByte) {
  ts.resetConfig();
  InstallEchoHandler();
  const std::string resp = SendInParts(port, kChunkedHead, "3\r\nfoo\r\n1;a=b\r\nX\r\n0\r\n\r\n", 1);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.ends_with("fooX")) << resp;
}

// A body of many chunks, each received on its own: the chunks already decoded are not decoded again, nor lost.
TEST(HttpParserChunkedResume, LargeBodyReceivedChunkByChunk) {
  ts.resetConfig();
  ts.router().setDefault([](const HttpRequestView& req) {
    return req.makeResponse(
        std::format("{}:{:x}:{}", req.body().size(), Fnv1a(req.body()), req.trailerValueOrEmpty("x-end")));
  });
  static constexpr std::size_t kNbChunks = 512;
  static constexpr std::size_t kChunkSize = 2048;

  std::string expectedBody;
  std::string chunkedBody;
  for (std::size_t chunkPos = 0; chunkPos < kNbChunks; ++chunkPos) {
    // Distinct contents: a chunk copied twice, or lost, changes the checksum.
    std::string chunk(kChunkSize, static_cast<char>('a' + (chunkPos % 26)));
    chunk.replace(0, 8, std::format("{:08d}", chunkPos));
    chunkedBody += std::format("{:x}\r\n", kChunkSize);
    chunkedBody += chunk;
    chunkedBody += "\r\n";
    expectedBody += chunk;
  }
  chunkedBody += "0\r\nX-End: done\r\n\r\n";

  // One chunk framing (size line, data and CRLF) per write, a little more than one per read.
  const std::string resp = SendInParts(port, kChunkedHead, chunkedBody, kChunkSize / 2 + 100, 100us);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.ends_with(std::format("{}:{:x}:done", expectedBody.size(), Fnv1a(expectedBody)))) << resp;
}

// A single chunk larger than the reads: its size line is the only part decoded again by each of them.
TEST(HttpParserChunkedResume, SingleLargeChunkReceivedInManyReads) {
  ts.resetConfig();
  ts.router().setDefault([](const HttpRequestView& req) {
    return req.makeResponse(std::format("{}:{:x}", req.body().size(), Fnv1a(req.body())));
  });
  std::string body(512UL * 1024UL, 'x');
  for (std::size_t bodyPos = 0; bodyPos < body.size(); bodyPos += 997) {
    body[bodyPos] = static_cast<char>('a' + (bodyPos % 26));
  }
  const std::string chunked = std::format("{:x}\r\n", body.size()) + body + "\r\n0\r\n\r\n";
  const std::string resp = SendInParts(port, kChunkedHead, chunked, 8192, 50us);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.ends_with(std::format("{}:{:x}", body.size(), Fnv1a(body)))) << resp;
}

// The requests following a chunked body received in several reads are served: its end is found at the right place.
TEST(HttpParserChunkedResume, PipelinedRequestsAfterBodyReceivedInSeveralReads) {
  ts.resetConfig();
  InstallEchoHandler();
  test::ClientConnection conn(port);
  NativeHandle fd = conn.fd();
  ASSERT_TRUE(SetTcpNoDelay(fd));
  test::sendAll(fd, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nA1" +
                        std::string("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nB2"));
  std::this_thread::sleep_for(20ms);
  test::sendAll(fd, "B\r\n3\r\nB3");
  std::this_thread::sleep_for(20ms);
  test::sendAll(fd, "B\r\n0\r\n\r\nPOST / HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 2\r\n\r\nC4");
  const std::string resp = test::recvUntilClosed(fd);
  EXPECT_EQ(test::countOccurrences(resp, "HTTP/1.1 200"), 3) << resp;
  EXPECT_LT(resp.find("A1"), resp.find("B2B"));
  EXPECT_LT(resp.find("B2B"), resp.find("C4"));
  EXPECT_TRUE(resp.contains("B2BB3B")) << resp;
}

// =============================================================================
// Lines of a chunked body are bounded by maxHeaderBytes, whatever the way they are received.
// =============================================================================

// A chunk-size line without end was buffered (and searched again by each read) without limit.
TEST(HttpParserChunkedLimits, UnterminatedChunkSizeLineIsBounded) {
  ts.resetConfig();
  InstallEchoHandler();
  const std::string resp = SendInParts(port, kChunkedHead, "1;" + std::string(3 * 3000, 'a'), 3000);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
  EXPECT_TRUE(resp.contains("Chunk size line too long")) << resp;
}

TEST(HttpParserChunkedLimits, UnterminatedChunkSizeLineReceivedAtOnceIsBounded) {
  ts.resetConfig();
  InstallEchoHandler();
  const std::string resp =
      test::sendAndCollect(port, std::string(kChunkedHead) + "1;" + std::string(64UL * 1024UL, 'a'));
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
  EXPECT_TRUE(resp.contains("Chunk size line too long")) << resp;
}

TEST(HttpParserChunkedLimits, TerminatedChunkSizeLineIsBounded) {
  ts.resetConfig();
  InstallEchoHandler();
  const std::string resp =
      test::sendAndCollect(port, std::string(kChunkedHead) + "1;" + std::string(10000, 'a') + "\r\nX\r\n0\r\n\r\n");
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
  EXPECT_TRUE(resp.contains("Chunk size line too long")) << resp;
}

// The limit includes the CRLF of the line, and does not depend on how the line is received.
TEST(HttpParserChunkedLimits, ChunkSizeLineBoundary) {
  static constexpr std::size_t kMaxHeaderBytes = 128;  // the minimum accepted value
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) { cfg.withMaxHeaderBytes(kMaxHeaderBytes); });
  InstallEchoHandler();

  for (const std::size_t lineSize : {kMaxHeaderBytes - 1, kMaxHeaderBytes, kMaxHeaderBytes + 1}) {
    SCOPED_TRACE(lineSize);
    // "5;" + extension + CRLF
    const std::string line = "5;" + std::string(lineSize - 4, 'e') + "\r\n";
    ASSERT_EQ(line.size(), lineSize);
    const bool accepted = lineSize <= kMaxHeaderBytes;
    const std::string chunked = line + "Hello\r\n0\r\n\r\n";

    for (const std::size_t partSize : {chunked.size(), std::size_t{50}, std::size_t{1}}) {
      SCOPED_TRACE(partSize);
      const std::string resp = SendInParts(port, kChunkedHead, chunked, partSize, 1ms);
      if (accepted) {
        ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
        EXPECT_TRUE(resp.ends_with("Hello")) << resp;
      } else {
        ASSERT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
      }
    }
  }
  ts.resetConfig();
}

// A trailer line without end was buffered (and parsed again by each read) without limit.
TEST(HttpParserChunkedLimits, UnterminatedTrailerLineIsBounded) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) { cfg.withMaxHeaderBytes(128); });
  InstallEchoHandler();
  const std::string resp = SendInParts(port, kChunkedHead, "3\r\nfoo\r\n0\r\nX-Big: " + std::string(300, 'V'), 100);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 431")) << resp;
  ts.resetConfig();
}

// A chunk making the body exceed its limit is rejected when announced, without waiting for (and buffering) its data.
TEST(HttpParserChunkedLimits, ChunkExceedingTheBodyLimitIsRejectedBeforeItsData) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) { cfg.withMaxBodyBytes(8U); });
  InstallEchoHandler();
  test::ClientConnection conn(port);
  NativeHandle fd = conn.fd();
  // 5 bytes received, then a chunk of 5 more is announced: 10 > 8. Its data is never sent.
  test::sendAll(fd, std::string(kChunkedHead) + "5\r\nhello\r\n5\r\n");
  const std::string resp = test::recvUntilClosed(fd, 3s);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 413")) << resp;
  ts.resetConfig();
}

// The body limit applies to the sum of the chunks received by several reads, and is exact.
TEST(HttpParserChunkedLimits, BodyLimitAppliesToChunksOfSeveralReads) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) { cfg.withMaxBodyBytes(10U); });
  InstallEchoHandler();
  const std::string atLimit = SendInParts(port, kChunkedHead, "5\r\nhello\r\n5\r\nworld\r\n0\r\n\r\n", 6, 3ms);
  ASSERT_TRUE(atLimit.starts_with("HTTP/1.1 200")) << atLimit;
  EXPECT_TRUE(atLimit.ends_with("helloworld")) << atLimit;

  const std::string overLimit =
      SendInParts(port, kChunkedHead, "5\r\nhello\r\n5\r\nworld\r\n1\r\n!\r\n0\r\n\r\n", 6, 3ms);
  ASSERT_TRUE(overLimit.starts_with("HTTP/1.1 413")) << overLimit;
  ts.resetConfig();
}

}  // namespace aeronet
