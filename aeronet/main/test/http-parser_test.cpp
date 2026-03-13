#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;

namespace aeronet {

namespace {

// Shared server used by most tests. A short poll interval keeps NeedMore round-trips fast.
test::TestServer ts(HttpServerConfig{}, {}, 5ms);
const auto port = ts.port();

// Echo the request body back in the response.
void InstallEchoHandler() {
  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });
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

}  // namespace

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
  ASSERT_TRUE(resp.contains("400")) << resp;
}

// Content-Length with trailing non-numeric chars → 400 Bad Request.
TEST(HttpParserFixedLength, InvalidContentLength_TrailingGarbage) {
  std::string req = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5abc\r\nConnection: close\r\n\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.contains("400")) << resp;
}

// Content-Length value exceeds configured maxBodyBytes → 413 Payload Too Large.
TEST(HttpParserFixedLength, ContentLengthExceedsMaxBodyBytes) {
  test::TestServer smallTs(HttpServerConfig{}.withMaxBodyBytes(16), {}, 5ms);
  smallTs.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 32\r\nConnection: close\r\n\r\n" + std::string(32, 'X');
  std::string resp = test::sendAndCollect(smallTs.port(), req);
  ASSERT_TRUE(resp.contains("413")) << resp;
}

// Expect: 100-continue with Content-Length: 0 — server must NOT send 100 Continue.
TEST(HttpParserFixedLength, Expect100Continue_ZeroBody_NoInterim) {
  InstallEchoHandler();
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_FALSE(resp.starts_with("HTTP/1.1 100")) << "Should not receive 100 Continue for zero-length body\n" << resp;
  ASSERT_TRUE(resp.contains("200")) << resp;
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

// Non-hexadecimal chunk size → 400 Bad Request.
TEST(HttpParserChunked, InvalidChunkSize_NonHex) {
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "XY\r\nbad\r\n"
      "0\r\n\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.contains("400")) << resp;
}

// Chunk size alone (without data bytes) exceeds maxBodyBytes → 413 Payload Too Large.
TEST(HttpParserChunked, ChunkSizeExceedsMaxBodyBytes) {
  test::TestServer smallTs(HttpServerConfig{}.withMaxBodyBytes(8), {}, 5ms);
  smallTs.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });
  // Declare a chunk of 32 bytes which exceeds the 8-byte limit.
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "20\r\n" +
      std::string(32, 'A') + "\r\n0\r\n\r\n";
  std::string resp = test::sendAndCollect(smallTs.port(), req);
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
  ts.router().setDefault([](const HttpRequest& req) {
    auto val = req.trailerValueOrEmpty("X-Checksum");
    return HttpResponse(std::string(val));
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

// NeedMore in trailer parsing: the trailer line arrives without its CRLF.
TEST(HttpParserChunkedTrailers, NeedMore_PartialTrailerLine) {
  ts.router().setDefault([](const HttpRequest& req) {
    auto val = req.trailerValueOrEmpty("X-Tag");
    return HttpResponse(std::string(val));
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

// A forbidden header used as a trailer → 400 Bad Request.
TEST(HttpParserChunkedTrailers, ForbiddenTrailerHeader) {
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "3\r\nfoo\r\n"
      "0\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n";
  std::string resp = test::sendAndCollect(port, req);
  ASSERT_TRUE(resp.contains("400")) << resp;
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
  test::TestServer smallTs(HttpServerConfig{}.withMaxHeaderBytes(128), {}, 5ms);
  smallTs.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });
  // Build a trailer value long enough to exceed the 128-byte limit.
  std::string longValue(256, 'V');
  std::string req =
      "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "3\r\nfoo\r\n"
      "0\r\n"
      "X-Big: " +
      longValue + "\r\n\r\n";
  std::string resp = test::sendAndCollect(smallTs.port(), req);
  ASSERT_TRUE(resp.contains("431")) << resp;
}

// =============================================================================
// Large body — ensure chunk processing with many segments works correctly.
// =============================================================================

TEST(HttpParserChunked, LargeBody_ManyChunks) {
  InstallEchoHandler();
  constexpr std::size_t kChunkDataSize = 512;
  constexpr int kChunkCount = 64;

  std::string req = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  std::string expected;
  char hex[16];
  for (int i = 0; i < kChunkCount; ++i) {
    std::string data(kChunkDataSize, static_cast<char>('A' + (i % 26)));
    std::snprintf(hex, sizeof(hex), "%zx", kChunkDataSize);
    req += std::string(hex) + "\r\n" + data + "\r\n";
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
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

}  // namespace aeronet
