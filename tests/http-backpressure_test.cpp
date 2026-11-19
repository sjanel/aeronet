#include <gtest/gtest.h>

#ifdef AERONET_POSIX
#include <netinet/in.h>
#include <sys/socket.h>  // NOLINT(misc-include-cleaner) socklen_t, SOL_SOCKET, SO_SNDBUF
#elifdef AERONET_WINDOWS
#include <winsock2.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aeronet/file.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/socket.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/zerocopy-mode.hpp"

using namespace std::chrono_literals;

namespace aeronet {

namespace {
test::TestServer ts;

// Receive buffer sizes for slow reader tests. The minimal one (raised to the kernel minimum), set before connecting,
// keeps the advertised window small: a few KiB of response then stay in the server send queue, still referencing the
// server buffers. Switching to the large one afterwards quickly delivers the rest.
// Some kernels (Linux 7.0) then deliver MSG_ZEROCOPY sends by a few hundred bytes per delayed ACK or retransmission
// timeout, even once the buffer is enlarged. The tests queueing large zerocopy responses rather fix the server send
// buffer (kSmallSendBufferBytes), the client receiving with the large buffer from the start: the server socket fills
// all the same, the kernel automatic tuning no longer growing its send buffer to the size of the response.
constexpr int kMinimalReceiveBufferSize = 1;
constexpr int kLargeReceiveBufferSize = 65536;
constexpr std::uint32_t kSmallSendBufferBytes = 64U << 10U;

// The route of the handler of test::EventLoopBlocker in the tests below.
constexpr std::string_view kBlockLoopPath = "/block-loop";

// Runs 'sendWhileBlocked' while the event loop of the server listening on 'serverPort' is blocked in the handler of
// 'blocker' (registered at kBlockLoopPath), then checks the response to the blocking request: the server reads all the
// bytes sent there in a single readable event.
void SendWhileEventLoopBlocked(uint16_t serverPort, test::EventLoopBlocker& blocker,
                               const std::function<void()>& sendWhileBlocked) {
  test::ClientConnection blockerCnx(serverPort);
  test::sendAll(blockerCnx.fd(),
                std::format("GET {} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", kBlockLoopPath));
  blocker.runWhileBlocked(sendWhileBlocked);
  EXPECT_TRUE(test::recvUntilClosed(blockerCnx.fd()).ends_with("blocked"));
}

}  // namespace

// The input of a connection closing after a response may still be in the socket rather than in its input buffer: here
// the read ending the request answered with Connection: close stops exactly at its end (a whole read of
// minReadChunkBytes), the next pipelined request staying in the socket. The connection lingers all the same: closed
// right away, the kernel would reset it, discarding the end of the response still in the socket send queue (the client
// window is small).
TEST(HttpLingeringClose, RequestsLeftInTheSocketBehindAClosingResponse) {
  HttpServerConfig cfg;
  cfg.withZerocopyMode(ZerocopyMode::Disabled);
  cfg.withSocketSendBufferBytes(512U << 10U);  // the whole response fits in the socket send queue
  test::TestServer server(std::move(cfg));
  const std::string big(256UL << 10U, 'r');
  test::EventLoopBlocker blocker;
  auto router = server.router();
  router.setPath(http::Method::GET, kBlockLoopPath, blocker.handler());
  router.setPath(http::Method::GET, "/s", [](const HttpRequestView& req) { return req.makeResponse("ok"); });
  router.setPath(http::Method::GET, "/big", [big](const HttpRequestView& req) { return req.makeResponse(big); });

  test::ClientConnection cnx(server.port(), 500ms, kMinimalReceiveBufferSize);
  const NativeHandle fd = cnx.fd();
  test::sendAll(fd, "GET /s HTTP/1.1\r\nHost: x\r\n\r\n");  // accepted before the event loop blocks
  ASSERT_TRUE(test::recvWithTimeout(fd, 2s).ends_with("ok"));

  static constexpr std::string_view kHeadStart = "GET /big HTTP/1.1\r\nHost: x\r\nConnection: close\r\nX-Pad: ";
  std::string closingRequest(kHeadStart);
  closingRequest.append(HttpServerConfig{}.minReadChunkBytes - kHeadStart.size() - http::DoubleCRLF.size(), 'p');
  closingRequest.append(http::DoubleCRLF);
  ASSERT_EQ(closingRequest.size(), HttpServerConfig{}.minReadChunkBytes);
  SendWhileEventLoopBlocked(server.port(), blocker,
                            [&] { test::sendAll(fd, closingRequest + "GET /s HTTP/1.1\r\nHost: x\r\n\r\n"); });
  std::this_thread::sleep_for(100ms);  // the server wrote the response, then closed
  test::SetReceiveBufferSize(fd, kLargeReceiveBufferSize);
  const test::ReceivedUntilEnd received = test::RecvUntilEnd(fd, 5s);
  EXPECT_TRUE(received.data.ends_with(big)) << "received " << received.data.size() << " bytes";
  EXPECT_EQ(received.end, test::ReceivedUntilEnd::End::Closed);

  ASSERT_TRUE(ShutdownWrite(fd));
  EXPECT_TRUE(test::DrainEndsWithin(server.server, 3s));
}

#ifdef AERONET_LINUX

// A client that stops reading a large response must not block the server: the response waits in the connection output
// buffer for the socket to become writable, while other connections keep being served.
TEST(HttpBackpressure, StalledReaderDoesNotBlockOtherClients) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) { cfg.withZerocopyMode(ZerocopyMode::Disabled); });
  const std::string big(2UL << 20U, 'b');
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/stalled-big",
                 [&big](const HttpRequestView& req) { return req.makeResponse(big); });
  router.setPath(http::Method::GET, "/small", [](const HttpRequestView& req) { return req.makeResponse("ok"); });

  test::ClientConnection stalled(ts.port(), 500ms, kMinimalReceiveBufferSize);
  test::sendAll(stalled.fd(), "GET /stalled-big HTTP/1.1\r\nhost: h\r\n\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds{100});  // the big response now waits for the stalled client

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/small";
  opt.recvTimeout = std::chrono::milliseconds{2000};
  const auto start = std::chrono::steady_clock::now();
  const std::string resp = test::requestOrThrow(ts.port(), opt);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.ends_with("ok")) << resp;
  EXPECT_LT(elapsed, std::chrono::seconds{1});
}

// A request pipelined behind a response that had to wait for the socket is served once that response is written: being
// already buffered, it does not produce another read event.
TEST(HttpBackpressure, PipelinedRequestBehindBufferedResponseIsServed) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) { cfg.withZerocopyMode(ZerocopyMode::Disabled); });
  const std::string big(1UL << 20U, 'p');
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/pipelined-big",
                 [&big](const HttpRequestView& req) { return req.makeResponse(big); });
  router.setPath(http::Method::GET, "/pipelined-small",
                 [](const HttpRequestView& req) { return req.makeResponse("ok"); });

  test::ClientConnection sock(ts.port(), 500ms, kMinimalReceiveBufferSize);
  const NativeHandle fd = sock.fd();
  test::sendAll(fd,
                "GET /pipelined-big HTTP/1.1\r\nhost: h\r\n\r\n"
                "GET /pipelined-small HTTP/1.1\r\nhost: h\r\nconnection: close\r\n\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds{100});  // the first response now waits for the socket
  test::SetReceiveBufferSize(fd, kLargeReceiveBufferSize);

  const std::string resp = test::recvUntilClosed(fd, std::chrono::seconds{3});
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp.substr(0, 64);
  const auto secondPos = resp.find("HTTP/1.1 200", 1);
  ASSERT_NE(secondPos, std::string::npos) << "second response missing, received " << resp.size() << " bytes";
  EXPECT_TRUE(std::string_view(resp).substr(0, secondPos).ends_with(big));
  EXPECT_TRUE(resp.ends_with("ok"));
}

// A streaming handler writing to a client that does not read is told when the buffered output exceeds
// maxOutboundBufferBytes (writeBody() returns false) instead of blocking the server until the client reads.
TEST(HttpBackpressure, StreamingWriterReportsOverflowForStalledReader) {
  static constexpr uint32_t kMaxOutboundBufferBytes = 256U << 10U;
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Disabled);
    cfg.withMaxOutboundBufferBytes(kMaxOutboundBufferBytes);
  });
  static constexpr int kMaxChunks = 1000;
  std::atomic<int> acceptedChunks{-1};
  ts.resetRouterAndGet().setPath(http::Method::GET, "/stream-stalled",
                                 [&acceptedChunks](const HttpRequestView&, HttpResponseWriter& writer) {
                                   const std::string chunk(64UL << 10U, 's');
                                   int nbAccepted = 0;
                                   while (nbAccepted < kMaxChunks && writer.writeBody(chunk)) {
                                     ++nbAccepted;
                                   }
                                   acceptedChunks.store(nbAccepted);
                                   writer.end();
                                 });

  test::ClientConnection stalled(ts.port(), 500ms, kMinimalReceiveBufferSize);
  test::sendAll(stalled.fd(), "GET /stream-stalled HTTP/1.1\r\nhost: h\r\n\r\n");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (acceptedChunks.load() < 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  // The output beyond the limit is refused: a few chunks only, not the 1000 of the handler.
  EXPECT_GE(acceptedChunks.load(), 0) << "the streaming handler did not return";
  EXPECT_LT(acceptedChunks.load(), kMaxChunks);
}

// A write beyond the declared Content-Length sends the bytes still due only
// (HttpStreaming.BodyBeyondItsDeclaredLengthIsCut). When the output limit refuses them too, the write fails all the
// same.
TEST(HttpBackpressure, WriteBeyondTheDeclaredLengthToAStalledReaderFails) {
  static constexpr uint32_t kMaxOutboundBufferBytes = 256U << 10U;
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Disabled);
    cfg.withMaxOutboundBufferBytes(kMaxOutboundBufferBytes);
    cfg.withSocketSendBufferBytes(kSmallSendBufferBytes);  // the kernel does not absorb the first write
  });
  static constexpr std::size_t kDeclaredLength = 1UL << 20U;
  std::atomic<int> accepted{-1};
  ts.resetRouterAndGet().setPath(http::Method::GET, "/beyond-stalled",
                                 [&accepted](const HttpRequestView&, HttpResponseWriter& writer) {
                                   writer.contentLength(kDeclaredLength);
                                   // Leaves output waiting for the socket, beyond which the limit applies.
                                   const std::string half(kDeclaredLength / 2U, 'a');
                                   writer.writeBody(half);
                                   accepted.store(writer.writeBody(half + 'b') ? 1 : 0);
                                   writer.end();
                                 });

  test::ClientConnection stalled(ts.port(), 500ms, kMinimalReceiveBufferSize);
  test::sendAll(stalled.fd(), "GET /beyond-stalled HTTP/1.1\r\nhost: h\r\n\r\n");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (accepted.load() < 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  EXPECT_EQ(accepted.load(), 0);
}

// With zerocopy, appending to an output buffer whose start was already sent with MSG_ZEROCOPY (here the chunks of a
// streaming response to a slow reader) must not reallocate the memory the kernel still reads: the whole body is
// received intact and in order.
TEST(HttpBackpressure, ZerocopyBufferedStreamingResponseIsIntact) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(1U);
    cfg.withSocketSendBufferBytes(kSmallSendBufferBytes);
  });
  static constexpr int kNbChunks = 16;
  static constexpr std::size_t kChunkSize = 64UL << 10U;
  std::string expectedBody;
  for (int chunkPos = 0; chunkPos < kNbChunks; ++chunkPos) {
    expectedBody.append(kChunkSize, static_cast<char>('a' + chunkPos));
  }
  ts.resetRouterAndGet().setPath(
      http::Method::GET, "/zerocopy-stream", [](const HttpRequestView&, HttpResponseWriter& writer) {
        for (int chunkPos = 0; chunkPos < kNbChunks; ++chunkPos) {
          if (!writer.writeBody(std::string(kChunkSize, static_cast<char>('a' + chunkPos)))) {
            break;
          }
        }
        writer.end();
      });

  test::ClientConnection sock(ts.port(), 500ms, kLargeReceiveBufferSize);
  const NativeHandle fd = sock.fd();
  test::sendAll(fd, "GET /zerocopy-stream HTTP/1.1\r\nhost: h\r\nconnection: close\r\n\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds{100});  // the chunks now accumulate behind the first one

  const std::string resp = test::recvUntilClosed(fd, std::chrono::seconds{5});
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp.substr(0, 64);
  EXPECT_EQ(test::parseResponseOrThrow(resp).body, expectedBody);
}

namespace {
// 'size' bytes whose value depends on their position and on 'seed': a misplaced byte is noticed.
std::string PatternBytes(std::size_t size, std::size_t seed) {
  std::string bytes(size, '\0');
  for (std::size_t pos = 0; pos < size; ++pos) {
    bytes[pos] = static_cast<char>('a' + (((pos * 7) + seed) % 26));
  }
  return bytes;
}
}  // namespace

// Responses to requests pipelined behind a response that waits for the socket, file responses among them, are received
// intact and in order: each file goes out after the head of its response, before the responses that follow.
TEST(HttpBackpressure, PipelinedFileResponsesBehindBufferedResponseAreIntact) {
  const std::string inMemoryBody = PatternBytes(1UL << 20U, 0);
  test::ScopedTempDir tmpDir;
  const std::string fileBodies[]{PatternBytes(300UL << 10U, 1), PatternBytes(10, 2), PatternBytes(1UL << 20U, 3)};
  const test::ScopedTempFile files[]{
      test::ScopedTempFile(tmpDir, fileBodies[0]),
      test::ScopedTempFile(tmpDir, fileBodies[1]),
      test::ScopedTempFile(tmpDir, fileBodies[2]),
  };
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/in-memory",
                 [&inMemoryBody](const HttpRequestView& req) { return req.makeResponse(inMemoryBody); });
  for (std::size_t filePos = 0; filePos < std::size(files); ++filePos) {
    router.setPath(http::Method::GET, std::format("/file/{}", filePos),
                   [path = files[filePos].filePath().string()](const HttpRequestView&) {
                     return HttpResponse(http::StatusCodeOK).file(File(path));
                   });
  }
  const std::string_view expectedBodies[]{inMemoryBody, fileBodies[0], fileBodies[1], inMemoryBody, fileBodies[2]};

  for (const ZerocopyMode mode : {ZerocopyMode::Disabled, ZerocopyMode::Enabled}) {
    ts.resetConfigAndPostUpdate([mode](HttpServerConfig& cfg) {
      cfg.withZerocopyMode(mode);
      cfg.withZerocopyMinBytes(1U);
      cfg.withSocketSendBufferBytes(kSmallSendBufferBytes);
    });
    test::ClientConnection sock(ts.port(), 500ms, kLargeReceiveBufferSize);
    const NativeHandle fd = sock.fd();
    test::sendAll(fd,
                  "GET /in-memory HTTP/1.1\r\nhost: h\r\n\r\n"
                  "GET /file/0 HTTP/1.1\r\nhost: h\r\n\r\n"
                  "GET /file/1 HTTP/1.1\r\nhost: h\r\n\r\n"
                  "GET /in-memory HTTP/1.1\r\nhost: h\r\n\r\n"
                  "GET /file/2 HTTP/1.1\r\nhost: h\r\nconnection: close\r\n\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});  // the first response now waits for the socket

    const std::string resp = test::recvUntilClosed(fd, std::chrono::seconds{5});
    std::string_view rest = resp;
    for (const std::string_view expectedBody : expectedBodies) {
      ASSERT_TRUE(rest.starts_with("HTTP/1.1 200")) << rest.substr(0, 64);
      const auto headEnd = rest.find(http::DoubleCRLF);
      ASSERT_NE(headEnd, std::string_view::npos);
      rest.remove_prefix(headEnd + http::DoubleCRLF.size());
      ASSERT_GE(rest.size(), expectedBody.size());
      // Not EXPECT_EQ: a difference would print megabytes.
      ASSERT_TRUE(rest.substr(0, expectedBody.size()) == expectedBody)
          << "response body differs, zerocopy mode " << static_cast<int>(mode);
      rest.remove_prefix(expectedBody.size());
    }
    EXPECT_TRUE(rest.empty()) << rest.size() << " unexpected bytes";
  }
}

namespace {

// A configuration whose responses quickly wait for the socket: a send buffer of a few KiB, without zerocopy.
HttpServerConfig BlockingOutputConfig() {
  HttpServerConfig cfg;
  cfg.withZerocopyMode(ZerocopyMode::Disabled);
  cfg.withSocketSendBufferBytes(4096);
  return cfg;
}

constexpr std::string_view kSmallRequest = "GET /s HTTP/1.1\r\nHost: x\r\n\r\n";

// The routes of the tests below: the blocking one of 'blocker', '/s' answering "ok", '/body' answering 'body' and
// '/file' answering the content of 'filePath' (if any).
void AddBlockingOutputRoutes(test::TestServer& server, test::EventLoopBlocker& blocker, const std::string& body,
                             std::string_view filePath = {}) {
  auto router = server.router();
  router.setPath(http::Method::GET, kBlockLoopPath, blocker.handler());
  router.setPath(http::Method::GET, "/s", [](const HttpRequestView& req) { return req.makeResponse("ok"); });
  router.setPath(http::Method::GET, "/body", [body](const HttpRequestView& req) { return req.makeResponse(body); });
  if (!filePath.empty()) {
    router.setPath(http::Method::GET, "/file", [path = std::string(filePath)](const HttpRequestView&) {
      return HttpResponse(http::StatusCodeOK).file(File(path));
    });
  }
}

// Connects a client with a minimal receive buffer (or 'receiveBufferSize'), with a first exchange: the server accepts
// the connection before its event loop blocks.
test::ClientConnection ConnectSlowReader(uint16_t serverPort, int receiveBufferSize = kMinimalReceiveBufferSize) {
  test::ClientConnection cnx(serverPort, 500ms, receiveBufferSize);
  test::sendAll(cnx.fd(), kSmallRequest);
  EXPECT_TRUE(test::recvWithTimeout(cnx.fd(), 2s).ends_with("ok"));
  return cnx;
}

// Pipelined requests for '/body', then 'nbSmallRequests' for '/s', the last one closing the connection.
std::string BodyThenSmallRequests(int nbSmallRequests) {
  std::string requests = "GET /body HTTP/1.1\r\nHost: x\r\n\r\n";
  for (int requestPos = 1; requestPos < nbSmallRequests; ++requestPos) {
    requests += kSmallRequest;
  }
  requests += "GET /s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  return requests;
}

// Sends 'requests' while the event loop of 'server' is blocked, lets it serve them (its output blocking), then reads
// until the connection closes: checks that all the responses come, the first one with 'body', without parser error.
void ExpectAllResponsesBehindBlockedOutput(test::TestServer& server, test::EventLoopBlocker& blocker, NativeHandle fd,
                                           const std::string& requests, const std::string& body,
                                           std::chrono::milliseconds blockedOutputDuration) {
  std::atomic<int> parserError{0};
  server.server.setParserErrorCallback([&parserError](http::StatusCode status) { parserError.store(status); });
  SendWhileEventLoopBlocked(server.port(), blocker, [fd, &requests] { test::sendAll(fd, requests); });
  std::this_thread::sleep_for(blockedOutputDuration);  // the server served the event, its output blocked
  test::SetReceiveBufferSize(fd, kLargeReceiveBufferSize);

  const test::ReceivedUntilEnd received = test::RecvUntilEnd(fd, 5s);
  server.server.setParserErrorCallback({});
  EXPECT_EQ(parserError.load(), 0);
  EXPECT_EQ(received.end, test::ReceivedUntilEnd::End::Closed);
  EXPECT_TRUE(received.data.contains(body))
      << "first response truncated: received " << received.data.size() << " bytes";
  EXPECT_EQ(test::countOccurrences(received.data, "HTTP/1.1 200"), test::countOccurrences(requests, "GET "));
}

}  // namespace

// Requests pipelined behind a response waiting for the socket are complete requests, only left unparsed by the output
// backpressure, which also stops the reading. Here they are more than one read (minReadChunkBytes) and more than
// maxHeaderBytes: read on, they would start the header read timer, then be taken for an incomplete head larger than
// maxHeaderBytes (414), the connection closing before the pending response is written.
TEST(HttpBackpressure, PipelinedRequestsBehindBlockedOutputAreNotAnOversizedHead) {
  test::EventLoopBlocker blocker;
  // The response blocks after a few KiB, far from the fairness budget (128 KiB): the read loop does not stop for it.
  test::TestServer server(BlockingOutputConfig());
  const std::string body(256UL << 10U, 'b');
  AddBlockingOutputRoutes(server, blocker, body);

  const test::ClientConnection cnx = ConnectSlowReader(server.port(), 4096);
  const std::string requests = BodyThenSmallRequests(600);  // ~20 KiB
  ASSERT_GT(requests.size(), 2 * HttpServerConfig{}.maxHeaderBytes);
  ExpectAllResponsesBehindBlockedOutput(server, blocker, cnx.fd(), requests, body, 100ms);
}

// Same with a header read timeout: read on, the requests left unparsed behind a response waiting for the socket would
// start the header read timer, then be answered 408 once it expired, the connection closing before the pending
// response is written.
TEST(HttpBackpressure, PipelinedRequestsBehindBlockedOutputAreNotTimedOut) {
  static constexpr std::chrono::milliseconds kHeaderReadTimeout{100};
  test::EventLoopBlocker blocker;
  HttpServerConfig cfg = BlockingOutputConfig();
  cfg.withHeaderReadTimeout(kHeaderReadTimeout);
  test::TestServer server(std::move(cfg));
  const std::string body(256UL << 10U, 'b');
  AddBlockingOutputRoutes(server, blocker, body);

  const test::ClientConnection cnx = ConnectSlowReader(server.port(), 4096);
  const std::string requests = BodyThenSmallRequests(200);  // ~6 KiB
  ASSERT_GT(requests.size(), HttpServerConfig{}.minReadChunkBytes);
  ASSERT_LT(requests.size(), HttpServerConfig{}.maxHeaderBytes);  // not the 414 of the test above
  ExpectAllResponsesBehindBlockedOutput(server, blocker, cnx.fd(), requests, body, 5 * kHeaderReadTimeout);
}

// A client may half-close its connection right after its request (HTTP/1.0 style clients, `nc -N`, some health
// checks): the end of its input must not cut the response short. Here the server reads the request with the end of the
// input in the same readable event (flagged RDHUP), and the response, buffered or a file, does not fit in the socket
// buffers: the reading stops while it waits for the socket, the end of the input being read once it is written.
// ZerocopyMode.HalfClosingClientReceivesWholeResponse covers zerocopy sends.
TEST(HttpBackpressure, HalfCloseRightAfterTheRequestStillGetsTheWholeResponse) {
  const std::string body(1UL << 20U, 'h');
  test::ScopedTempDir tmpDir;
  const test::ScopedTempFile file(tmpDir, body);
  test::EventLoopBlocker blocker;
  test::TestServer server(BlockingOutputConfig());
  AddBlockingOutputRoutes(server, blocker, body, file.filePath().string());

  for (const std::string_view target : {"/body", "/file"}) {
    SCOPED_TRACE(target);
    const test::ClientConnection cnx = ConnectSlowReader(server.port());
    const NativeHandle fd = cnx.fd();
    SendWhileEventLoopBlocked(server.port(), blocker, [fd, target] {
      test::sendAll(fd, std::format("GET {} HTTP/1.1\r\nHost: x\r\n\r\n", target));
      ASSERT_TRUE(ShutdownWrite(fd));
    });
    std::this_thread::sleep_for(100ms);  // the server served the event
    test::SetReceiveBufferSize(fd, kLargeReceiveBufferSize);
    const test::ReceivedUntilEnd received = test::RecvUntilEnd(fd, 5s);
    EXPECT_TRUE(received.data.ends_with(body)) << "received " << received.data.size() << " bytes";
    EXPECT_EQ(received.end, test::ReceivedUntilEnd::End::Closed);
  }
}

// Same for small pipelined requests: their responses are batched, then written at once, a write that blocks. Here the
// input ends with an incomplete request (that the client gives up), so that the batch is written after the requests
// were served, the end of the input still to read.
TEST(HttpBackpressure, HalfCloseRightAfterPipelinedRequestsStillGetsAllTheirResponses) {
  static constexpr int kNbRequests = 20;
  test::EventLoopBlocker blocker;
  test::TestServer server(BlockingOutputConfig());
  // 20 responses: more than the socket buffers, less than the largest batch (64 KiB).
  const std::string body(2048, 'm');
  AddBlockingOutputRoutes(server, blocker, body);

  const test::ClientConnection cnx = ConnectSlowReader(server.port());
  const NativeHandle fd = cnx.fd();
  std::string requests;
  for (int requestPos = 0; requestPos < kNbRequests; ++requestPos) {
    requests += "GET /body HTTP/1.1\r\nHost: x\r\n\r\n";
  }
  requests += "GET /body HTTP/1.1\r\nHo";
  SendWhileEventLoopBlocked(server.port(), blocker, [fd, &requests] {
    test::sendAll(fd, requests);
    ASSERT_TRUE(ShutdownWrite(fd));
  });
  std::this_thread::sleep_for(100ms);  // the server served the event
  test::SetReceiveBufferSize(fd, kLargeReceiveBufferSize);
  const test::ReceivedUntilEnd received = test::RecvUntilEnd(fd, 5s);
  EXPECT_EQ(test::countOccurrences(received.data, "HTTP/1.1 200"), kNbRequests);
  EXPECT_TRUE(received.data.ends_with(body)) << "received " << received.data.size() << " bytes";
  EXPECT_EQ(received.end, test::ReceivedUntilEnd::End::Closed);
}

// A client half-closing its connection while its response waits for the socket: the end of its input, reported by a
// later readable event, is only read once the response is written, whether it is buffered or a file.
TEST(HttpBackpressure, HalfCloseWhileTheResponseWaitsForTheSocketStillGetsTheWholeResponse) {
  const std::string body(1UL << 20U, 'w');
  test::ScopedTempDir tmpDir;
  const test::ScopedTempFile file(tmpDir, body);
  test::EventLoopBlocker blocker;
  test::TestServer server(BlockingOutputConfig());
  AddBlockingOutputRoutes(server, blocker, body, file.filePath().string());

  for (const std::string_view target : {"/body", "/file"}) {
    SCOPED_TRACE(target);
    test::ClientConnection cnx(server.port(), 500ms, kMinimalReceiveBufferSize);
    const NativeHandle fd = cnx.fd();
    test::sendAll(fd, std::format("GET {} HTTP/1.1\r\nHost: x\r\n\r\n", target));
    std::this_thread::sleep_for(100ms);  // the response now waits for the socket
    ASSERT_TRUE(ShutdownWrite(fd));
    std::this_thread::sleep_for(100ms);  // the server handled the readable event of the half-close
    test::SetReceiveBufferSize(fd, kLargeReceiveBufferSize);
    const test::ReceivedUntilEnd received = test::RecvUntilEnd(fd, 5s);
    EXPECT_TRUE(received.data.ends_with(body)) << "received " << received.data.size() << " bytes";
    EXPECT_EQ(received.end, test::ReceivedUntilEnd::End::Closed);
  }
}

#ifdef AERONET_POSIX
namespace {
// SO_SNDBUF of a socket as reported by the kernel, or -1.
int SendBufferSize(int fd) {
  int bytes = -1;
  socklen_t len = sizeof(bytes);  // NOLINT(misc-include-cleaner)
  // NOLINTNEXTLINE(misc-include-cleaner)
  return ::getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, &len) == 0 ? bytes : -1;
}

// SO_SNDBUF of the server end, in this process, of a new connection to 'ts' whose GET 'path' is answered with a 200,
// or -1: the socket whose local and peer ports are the peer and local ports of the client one (other connections of the
// process may share one). The response is awaited first: the server sets the buffer right after accepting the
// connection, so reading it as soon as the accepted socket exists may see the system default.
int AcceptedSendBufferSize(std::string_view path) {
  test::ClientConnection sock(ts.port());
  test::sendAll(sock.fd(), std::format("GET {} HTTP/1.1\r\nhost: h\r\n\r\n", path));
  if (!test::recvWithTimeout(sock.fd(), 2s).starts_with("HTTP/1.1 200")) {
    return -1;
  }
  sockaddr_in client{};
  sockaddr_in server{};
  socklen_t clientLen = sizeof(client);  // NOLINT(misc-include-cleaner)
  socklen_t serverLen = sizeof(server);  // NOLINT(misc-include-cleaner)
  if (::getsockname(sock.fd(), reinterpret_cast<sockaddr*>(&client), &clientLen) != 0 ||
      ::getpeername(sock.fd(), reinterpret_cast<sockaddr*>(&server), &serverLen) != 0) {
    return -1;
  }
  for (int fd = 0; fd < 4096; ++fd) {
    sockaddr_in local{};
    sockaddr_in peer{};
    socklen_t localLen = sizeof(local);  // NOLINT(misc-include-cleaner)
    socklen_t peerLen = sizeof(peer);    // NOLINT(misc-include-cleaner)
    if (fd != sock.fd() && ::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &localLen) == 0 &&
        ::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peerLen) == 0 && local.sin_family == AF_INET &&
        local.sin_port == server.sin_port && peer.sin_port == client.sin_port) {
      return SendBufferSize(fd);
    }
  }
  return -1;
}
}  // namespace

// The send buffer of the accepted connections is the configured one, and the system default again for the connections
// accepted after a configuration update back to 0.
TEST(HttpServerSocketOptions, SendBufferOfAcceptedConnections) {
  static constexpr uint16_t kSendBufferBytes = 48U << 10U;  // no system default is that
  Socket probe(Socket::Type::Stream);
  ASSERT_TRUE(SetSendBufferSize(probe.fd(), kSendBufferBytes));
  const int expected = SendBufferSize(probe.fd());  // as stored by the kernel (Linux doubles it)
  ASSERT_GT(expected, 0);
  ts.resetRouterAndGet().setPath(http::Method::GET, "/sndbuf",
                                 [](const HttpRequestView&) { return HttpResponse("ok"); });

  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) { cfg.withSocketSendBufferBytes(kSendBufferBytes); });
  EXPECT_EQ(AcceptedSendBufferSize("/sndbuf"), expected);

  ts.resetConfigAndPostUpdate([](HttpServerConfig&) {});
  const int defaultSize = AcceptedSendBufferSize("/sndbuf");
  EXPECT_GT(defaultSize, 0);
  EXPECT_NE(defaultSize, expected);
}
#endif

// A large response streamed to a slow reader in chunks of various sizes, waiting in the connection output (the small
// ones merged, the others queued on their own and written several at once, with MSG_ZEROCOPY when enabled), is received
// intact.
TEST(HttpBackpressure, StreamedResponseToSlowReaderIsIntact) {
  static constexpr std::size_t kChunkSizes[]{1, 100, 4000, 5000, 70000, 3, 4096, 200000, 17, 65536};
  static constexpr int kNbRounds = 8;
  std::string expectedBody;
  for (int round = 0; round < kNbRounds; ++round) {
    for (const std::size_t chunkSize : kChunkSizes) {
      expectedBody += PatternBytes(chunkSize, expectedBody.size());
    }
  }
  ts.resetRouterAndGet().setPath(http::Method::GET, "/stream-slow-reader",
                                 [&expectedBody](const HttpRequestView&, HttpResponseWriter& writer) {
                                   std::string_view rest = expectedBody;
                                   for (std::size_t chunkPos = 0; !rest.empty(); ++chunkPos) {
                                     const std::size_t chunkSize = kChunkSizes[chunkPos % std::size(kChunkSizes)];
                                     if (!writer.writeBody(rest.substr(0, chunkSize))) {
                                       break;
                                     }
                                     rest.remove_prefix(chunkSize);
                                   }
                                   writer.end();
                                 });

  for (const ZerocopyMode mode : {ZerocopyMode::Disabled, ZerocopyMode::Enabled}) {
    ts.resetConfigAndPostUpdate([mode](HttpServerConfig& cfg) {
      cfg.withZerocopyMode(mode);
      cfg.withZerocopyMinBytes(16U << 10U);
    });
    test::ClientConnection sock(ts.port(), 500ms, kLargeReceiveBufferSize);
    const NativeHandle fd = sock.fd();
    test::setRecvTimeout(fd, std::chrono::milliseconds{2000});
    test::sendAll(fd, "GET /stream-slow-reader HTTP/1.1\r\nhost: h\r\nconnection: close\r\n\r\n");

    // Reads at a limited rate, until the server closes the connection.
    std::string resp;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < deadline && test::RecvAppend(fd, resp, 16UL << 10U) > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds{200});
    }
    ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp.substr(0, 64);
    const std::string body = test::parseResponseOrThrow(resp).body;
    ASSERT_EQ(body.size(), expectedBody.size()) << "zerocopy mode " << static_cast<int>(mode);
    // Not EXPECT_EQ: a difference would print megabytes.
    EXPECT_TRUE(body == expectedBody) << "response body differs, zerocopy mode " << static_cast<int>(mode);
  }
}

// A handler streaming a long response in small chunks without returning to the event loop keeps writing it once the
// socket was full: its output, waiting for the writable event that the event loop cannot process meanwhile, is written
// as the peer reads instead of piling up until the outbound limit closes the connection.
TEST(HttpBackpressure, LongStreamingHandlerKeepsWritingOnceTheSocketWasFull) {
  static constexpr std::size_t kChunkSize = 1024;
  static constexpr std::size_t kBodySize = 32UL << 20U;
  // At most 128 MiB/s: the client keeps up once it reads.
  static constexpr std::size_t kBytesPerMillisecond = 128UL << 10U;
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    // Far below the body, but above what the handler writes before the client reads.
    cfg.withMaxOutboundBufferBytes(12U << 20U);
  });
  ts.resetRouterAndGet().setPath(http::Method::GET, "/long-stream",
                                 [](const HttpRequestView&, HttpResponseWriter& writer) {
                                   const std::string chunk(kChunkSize, 'L');
                                   for (std::size_t written = 0; written < kBodySize; written += kChunkSize) {
                                     if (!writer.writeBody(chunk)) {
                                       return;
                                     }
                                     if (written % kBytesPerMillisecond == 0) {
                                       std::this_thread::sleep_for(std::chrono::milliseconds{1});
                                     }
                                   }
                                   writer.end();
                                 });

  // Above the loopback MSS: a smaller window would make the transfer crawl.
  test::ClientConnection sock(ts.port(), 500ms, 256 * 1024);
  const NativeHandle fd = sock.fd();
  test::sendAll(fd, "GET /long-stream HTTP/1.1\r\nhost: h\r\nconnection: close\r\n\r\n");
  // The handler fills the socket before the client reads.
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  const std::string resp = test::recvUntilClosed(fd, std::chrono::seconds{10});
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp.substr(0, 64);
  EXPECT_EQ(static_cast<std::size_t>(std::ranges::count(resp, 'L')), kBodySize);
  EXPECT_TRUE(resp.ends_with(http::EndChunk));
}

#endif  // AERONET_LINUX

}  // namespace aeronet
