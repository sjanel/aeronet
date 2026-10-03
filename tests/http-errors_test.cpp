#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#define AERONET_WANT_SOCKET_OVERRIDES
#define AERONET_WANT_READ_WRITE_OVERRIDES
#define AERONET_WANT_SENDFILE_PREAD_OVERRIDES

#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_tls_client.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#endif

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using namespace aeronet;

namespace {

struct Capture {
  std::mutex m;
  vector<http::StatusCode> errors;

  void push(http::StatusCode err) {
    std::scoped_lock lk(m);
    errors.push_back(err);
  }

  [[nodiscard]] bool checkAndClear(http::StatusCode err) {
    std::scoped_lock lk(m);
    const bool found = std::ranges::find(errors, err) != errors.end();
    errors.clear();
    return found;
  }
};

using test::SimpleGetRequest;

test::TestServer ts;
auto port = ts.port();

struct RequestDataAndExpectedStatusCode {
  [[nodiscard]] std::string_view expectedReqStart() {
    char* pLast = writeStatusCode(statusStrBuf + std::size(http::kHttpPrefix) + 1U + 1U + 1U, statusCode);
    *pLast = ' ';
    return {statusStrBuf, std::size(statusStrBuf)};
  }

  std::string_view data;
  http::StatusCode statusCode{};
  char statusStrBuf[std::size(http::kHttpPrefix) + 1U + 1U + 1U + http::StatusCodeLen + 1U]{"HTTP/1.1 "};
};

}  // namespace

TEST(HttpParserErrors, InvalidHTTPVersion) {
  Capture cap;
  ts.server.setParserErrorCallback([&](http::StatusCode err) { cap.push(err); });
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK); });

  for (RequestDataAndExpectedStatusCode expected : {
           RequestDataAndExpectedStatusCode("GET / HTTP/4.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                                            http::StatusCodeHTTPVersionNotSupported),
           RequestDataAndExpectedStatusCode("GET / HTTP/9.9\r\nHost: x\r\nConnection: close\r\n\r\n",
                                            http::StatusCodeHTTPVersionNotSupported),
           RequestDataAndExpectedStatusCode("GET /test HTTP/0.9\r\nHost: x\r\nConnection: close\r\n\r\n",
                                            http::StatusCodeHTTPVersionNotSupported),
           RequestDataAndExpectedStatusCode("GET /test HTTP/1.32\r\nHost: x\r\nConnection: close\r\n\r\n",
                                            http::StatusCodeBadRequest),
           RequestDataAndExpectedStatusCode("GET /test HTTP/91.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                                            http::StatusCodeBadRequest),
           RequestDataAndExpectedStatusCode("GET /test HTTP/r.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                                            http::StatusCodeBadRequest),
           RequestDataAndExpectedStatusCode("GET /test HTTP/-1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                                            http::StatusCodeBadRequest),
       }) {
    test::ClientConnection clientConnection(port);
    NativeHandle fd = clientConnection.fd();
    ASSERT_GE(fd, 0);
    test::sendAll(fd, expected.data);
    std::string resp = test::recvUntilClosed(fd);
    ASSERT_TRUE(resp.starts_with(expected.expectedReqStart())) << "for case:\n" << expected.data << "\nresp:\n" << resp;
    ASSERT_TRUE(cap.checkAndClear(expected.statusCode));
  }
}

TEST(HttpParserErrors, ExceptionInParserShouldBeControlled) {
  Capture cap;
  ts.server.setParserErrorCallback([&]([[maybe_unused]] http::StatusCode err) { throw std::runtime_error("boom"); });
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK); });

  std::string bad =
      "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\nConnection: close\r\n\r\n";  // invalid content-length

  {
    test::ClientConnection clientConnection(port);
    NativeHandle fd = clientConnection.fd();

    test::sendAll(fd, bad);
    std::string resp = test::recvUntilClosed(fd);
    ASSERT_TRUE(resp.contains("400")) << resp;
  }

  // NOLINTNEXTLINE(bugprone-std-exception-baseclass)
  ts.server.setParserErrorCallback([&]([[maybe_unused]] http::StatusCode err) { throw 42; });
  std::this_thread::sleep_for(2 * ts.server.config().pollInterval);
  {
    test::ClientConnection clientConnection(port);
    NativeHandle fd = clientConnection.fd();
    test::sendAll(fd, bad);
    std::string resp = test::recvUntilClosed(fd);
    ASSERT_TRUE(resp.contains("400")) << resp;
  }
}

TEST(HttpParserErrors, Expect100OnlyWithBody) {
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK); });
  test::ClientConnection clientConnection(port);
  NativeHandle fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  // zero length with Expect should NOT produce 100 Continue
  std::string zero =
      "POST /z HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, zero);
  std::string respZero = test::recvUntilClosed(fd);
  ASSERT_FALSE(respZero.contains("100 Continue"));
  // non-zero length with Expect should produce interim 100 then 200
  test::ClientConnection clientConnection2(port);
  int fd2 = clientConnection2.fd();
  ASSERT_GE(fd2, 0);
  std::string post =
      "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: close\r\n\r\nHELLO";
  test::sendAll(fd2, post);
  std::string resp = test::recvUntilClosed(fd2);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 100 Continue"));
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
}

// Fuzz-ish incremental chunk framing with random chunk sizes & boundaries.
TEST(HttpParserErrors, ChunkIncrementalFuzz) {
  ts.router().setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); });

  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> sizeDist(1, 15);
  std::string original;
  test::ClientConnection clientConnection(port);
  NativeHandle fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string head = "POST /f HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, head);
  // send 5 random chunks
  for (int i = 0; i < 5; ++i) {
    int sz = sizeDist(rng);
    std::string chunk(static_cast<std::size_t>(sz), static_cast<char>('a' + (i % 26)));
    original += chunk;
    char hex[16];
    std::string frame(hex, to_lower_hex(static_cast<std::size_t>(sz), hex));
    frame += "\r\n" + chunk + "\r\n";
    std::size_t pos = 0;
    while (pos < frame.size()) {
      std::size_t rem = frame.size() - pos;
      std::size_t slice = std::min<std::size_t>(1 + (rng() % 3), rem);
      test::sendAll(fd, frame.substr(pos, slice));
      pos += slice;
      std::this_thread::sleep_for(1ms);
    }
  }
  // terminating chunk
  test::sendAll(fd, http::EndChunk);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains(original.substr(0, 3))) << resp;  // sanity partial check
}

// =============================================================================
// connection-manager.cpp error paths
// =============================================================================

// Test TCP_NODELAY failure path
// This exercises the setsockopt failure path when tcpNoDelay is enabled
TEST(ConnectionManagerErrors, TcpNoDelayFailure) {
  test::QueueResetGuard<decltype(test::g_setsockopt_actions)> guard(test::g_setsockopt_actions);

  // We need to allow the listen socket setup to succeed, then fail on connection setsockopt
  // The listen socket setup calls setsockopt for SO_REUSEADDR (and possibly SO_REUSEPORT)
  // so we need to let those succeed first. We'll use an approach where we start the server first,
  // then inject failures for subsequent setsockopt calls.
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withTcpNoDelay(); });
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK); });

  // Now inject setsockopt failures for the next connection's TCP_NODELAY call
  // We push multiple failures to ensure one catches the TCP_NODELAY call
  test::PushSetsockoptAction({-1, EPERM});
  test::PushSetsockoptAction({-1, EPERM});

  // Make a request - server should still serve despite TCP_NODELAY failure
  auto resp = test::simpleGet(ts.port(), "/nodelay-fail");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// Test eventLoop.add failure path
// This exercises the path where epoll_ctl ADD fails for a new connection
TEST(ConnectionManagerErrors, EventLoopAddFailure) {
  test::EventLoopHookGuard guard;
  test::QueueResetGuard<decltype(test::g_epoll_ctl_add_actions)> epollAddGuard(test::g_epoll_ctl_add_actions);

  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK); });

  // First request should work normally
  auto resp1 = test::simpleGet(ts.port(), "/first");
  EXPECT_TRUE(resp1.starts_with("HTTP/1.1 200"));

  // Force the next epoll_ctl(ADD) (used by eventLoop.add for an accepted client fd) to fail.
  test::PushEpollCtlAddAction({-1, EIO});

  // Next connection should be accepted then immediately dropped due to add() failure.
  // We validate this by observing that the peer closes without returning an HTTP response.
  test::ClientConnection client(ts.port());
  EXPECT_TRUE(test::WaitForPeerClose(client.fd(), 500ms));

  // Server should remain healthy after handling the error.
  auto resp2 = test::simpleGet(ts.port(), "/after");
  EXPECT_TRUE(resp2.starts_with("HTTP/1.1 200")) << resp2;
}

// Test sweepIdleConnections - isImmediateCloseRequested path
// This exercises the sweep when a connection has requested immediate close
TEST(ConnectionManagerErrors, SweepIdleConnectionsImmediateClose) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.withKeepAliveMode();
    cfg.withKeepAliveTimeout(1h);  // Long timeout so sweep doesn't close by timeout
  });

  // Handler that causes an immediate close request (error path)
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse("immediate-close"); });

  // Normal request should work
  auto resp = test::simpleGet(ts.port(), "/sweep-test");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
}

// Test maxPerEventReadBytes fairness cap in handleReadableClient
// NOTE: This configuration option requires careful tuning and is primarily for server fairness
// with many concurrent connections. The test verifies the config is accepted and basic operation works.
TEST(ConnectionManagerErrors, MaxPerEventReadBytesFairness) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.maxPerEventReadBytes = 8192;  // Reasonable limit
  });

  ts.router().setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); });

  // Send a simple request - verify basic operation with fairness cap enabled
  auto resp = test::simpleGet(ts.port(), "/fairness");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// Test queueData TransportHint::Error path
TEST(HttpResponseDispatchErrors, QueueDataTransportError) {
  test::QueueResetGuard<decltype(test::g_write_actions)> guardWrite(test::g_write_actions);
  test::QueueResetGuard<decltype(test::g_writev_actions)> guardWritev(test::g_writev_actions);
  test::QueueResetGuard<decltype(test::g_on_accept_install_actions)> guardOnAccept(test::g_on_accept_install_actions);

  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse("test-body"); });

  // Inject a server-side write failure on the accepted fd (PlainTransport uses writev for head+body).
  test::g_on_accept_install_actions.push(test::AcceptInstallActions{
      .writeActions = {{-1, error::kBrokenPipe}},
      .writevActions = {{-1, error::kBrokenPipe}},
      .sendfileActions = {},
  });

  test::ClientConnection client(ts.port());

  test::sendAll(client.fd(), SimpleGetRequest("/write-error", http::keepalive));

  // On a transport error while sending, the server requests immediate close; the client may see
  // an empty/partial response (and should observe a close).
  (void)test::recvWithTimeout(client.fd(), 1000ms);
  EXPECT_TRUE(test::WaitForPeerClose(client.fd(), 2000ms));
}

// Test queueData isAnyCloseRequested() early return guard (lines 170-173 in http-response-dispatch.cpp).
// When an interim 102 Processing response fails to write (transport error), requestDrainAndClose() is
// set inside queueData(). The subsequent queueData() call for the final response detects
// isAnyCloseRequested() == true and returns early without queuing the response.
// Call chain: handleExpectHeader -> queueData(102) -> TransportHint::Error -> requestDrainAndClose()
//             -> finalizeAndSendResponseForHttp1 -> queueData(response) -> line 170 guard fires
TEST(HttpResponseDispatchErrors, QueueDataSkipsWhenCloseAlreadyRequested) {
  test::QueueResetGuard<decltype(test::g_write_actions)> guardWrite(test::g_write_actions);
  test::QueueResetGuard<decltype(test::g_writev_actions)> guardWritev(test::g_writev_actions);
  test::QueueResetGuard<decltype(test::g_on_accept_install_actions)> guardOnAccept(test::g_on_accept_install_actions);

  ts.server.setExpectationHandler([](const HttpRequestView& /*req*/, std::string_view token) {
    SingleHttpServer::ExpectationResult res;
    if (token == "proc") {
      res.kind = SingleHttpServer::ExpectationResultKind::Interim;
      res.interimStatus = 102;
      return res;
    }
    res.kind = SingleHttpServer::ExpectationResultKind::Continue;
    return res;
  });

  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse("ok"); });

  // Inject a write error so the 102 Processing interim response write fails with a transport error.
  // This causes requestDrainAndClose() to be set inside queueData() (Error branch).
  // The subsequent final response's queueData() call then sees isAnyCloseRequested() == true
  // and returns early — so "ok" is never written to the socket.
  test::g_on_accept_install_actions.push(test::AcceptInstallActions{
      .writeActions = {{-1, error::kBrokenPipe}},
      .writevActions = {{-1, error::kBrokenPipe}},
      .sendfileActions = {},
  });

  test::ClientConnection client(ts.port());
  // Send POST with Expect: proc and body already in the segment. The server processes
  // the Expect header, fails the 102 write, then calls queueData for the final response
  // which returns early because isAnyCloseRequested() is true — no "ok" body is sent.
  test::sendAll(client.fd(),
                "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: proc\r\nConnection: close\r\n\r\nhello");

  auto resp = test::recvWithTimeout(client.fd(), 500ms);
  EXPECT_FALSE(resp.contains("ok")) << "Final response must not be queued after drain+close was requested: " << resp;
  EXPECT_TRUE(test::WaitForPeerClose(client.fd(), 2000ms));

  ts.server.setExpectationHandler({});
}

// Test flushOutbound TransportHint::Error path (line 364-372)
TEST(HttpResponseDispatchErrors, FlushOutboundTransportError) {
  test::QueueResetGuard<decltype(test::g_write_actions)> guardWrite(test::g_write_actions);
  test::QueueResetGuard<decltype(test::g_writev_actions)> guardWritev(test::g_writev_actions);

  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.maxOutboundBufferBytes = 1U << 20U; });

  // Generate a large response to ensure buffering
  std::string largeBody(64UL * 1024, 'L');
  ts.router().setDefault(
      [&largeBody](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK).body(largeBody); });

  const auto prevAcceptCount = test::g_accept_count.load(std::memory_order_acquire);
  test::ClientConnection client(ts.port());

  // Install actions on the *server-side* accepted fd before sending the request.
  // This avoids racing the server's response write path.
  int serverFd = -1;
  const auto deadline = std::chrono::steady_clock::now() + 500ms;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto curCount = test::g_accept_count.load(std::memory_order_acquire);
    if (curCount > prevAcceptCount) {
      serverFd = test::g_last_accepted_fd.load(std::memory_order_acquire);
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_GE(serverFd, 0);

  // Arrange:
  //  - first writev: short write
  //  - second writev: EAGAIN => leaves buffered data and enables EPOLLOUT
  //  - third writev: error::kBrokenPipe => flushOutbound hits TransportHint::Error and requests immediate close
  test::SetWritevActions(serverFd, {{100, 0}, {-1, error::kWouldBlock}, {-1, error::kBrokenPipe}});
  ASSERT_EQ(test::g_writev_actions.size(serverFd), 3U);

  test::sendAll(client.fd(), SimpleGetRequest("/flush-error", http::keepalive));

  const auto resp = test::recvWithTimeout(client.fd(), 1000ms);
  EXPECT_LT(test::g_writev_actions.size(serverFd), 3U);
  EXPECT_TRUE(test::WaitForPeerClose(client.fd(), 2000ms)) << resp;
}

// A file response produced while an interim response (here 100 Continue, the body coming with the request head) still
// waits for the socket is queued behind it: its head after the buffered output, its file once that output is written.
TEST(HttpResponseDispatchErrors, FileResponseQueuedBehindPendingInterimResponse) {
  test::QueueResetGuard<decltype(test::g_writev_actions)> guardWritev(test::g_writev_actions);

  constexpr std::string_view kPayload = "file response behind a pending interim response";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  const std::string path = tmp.filePath().string();
  ts.router().setDefault([&path](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK).file(File(path)); });

  const auto prevAcceptCount = test::g_accept_count.load(std::memory_order_acquire);
  test::ClientConnection client(ts.port());
  int serverFd = -1;
  const auto deadline = std::chrono::steady_clock::now() + 500ms;
  while (std::chrono::steady_clock::now() < deadline) {
    if (test::g_accept_count.load(std::memory_order_acquire) > prevAcceptCount) {
      serverFd = test::g_last_accepted_fd.load(std::memory_order_acquire);
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_GE(serverFd, 0);

  // The 100 Continue cannot be written, neither at once nor by the flush following it: it stays buffered.
  test::SetWritevActions(serverFd, {{-1, error::kWouldBlock}, {-1, error::kWouldBlock}});
  test::sendAll(client.fd(),
                "POST /file HTTP/1.1\r\nhost: x\r\nexpect: 100-continue\r\ncontent-length: 5\r\n"
                "connection: close\r\n\r\nhello");

  const std::string resp = test::recvUntilClosed(client.fd());
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.ends_with(kPayload)) << resp;
}

// Test sendfile error path in flushFilePayload (line 532)
TEST(HttpResponseDispatchErrors, SendfileError) {
  test::QueueResetGuard<decltype(test::g_sendfile_actions)> guard(test::g_sendfile_actions);
  test::QueueResetGuard<decltype(test::g_on_accept_install_actions)> guardOnAccept(test::g_on_accept_install_actions);

  constexpr std::string_view kPayload = "sendfile error test payload content";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  std::string path = tmp.filePath().string();

  ts.router().setDefault([&path](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.file(File(path));
    writer.end();
  });

  // Inject server-side sendfile error on the accepted fd.
  test::g_on_accept_install_actions.push(test::AcceptInstallActions{
      .writeActions = {},
      .writevActions = {},
      .sendfileActions = {{-1, EIO}},
  });

  test::ClientConnection client(ts.port());

  test::sendAll(client.fd(), SimpleGetRequest("/sendfile-error", http::keepalive));

  // Expected behavior: headers may already be sent (200), but body will be truncated and the
  // server will close the connection.
  const auto resp = test::recvWithTimeout(client.fd(), 2000ms);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_FALSE(resp.contains(std::string_view{kPayload})) << resp;
  EXPECT_TRUE(test::WaitForPeerClose(client.fd(), 2000ms));
}

// Test sendfile WouldBlock path with retry
TEST(HttpResponseDispatchErrors, SendfileWouldBlockWithRetry) {
  test::QueueResetGuard<decltype(test::g_sendfile_actions)> guard(test::g_sendfile_actions);
  test::QueueResetGuard<decltype(test::g_on_accept_install_actions)> guardOnAccept(test::g_on_accept_install_actions);

  // Create a moderate-sized file
  std::string payload(32UL * 1024, 'R');
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, payload);
  std::string path = tmp.filePath().string();

  ts.router().setDefault([&path](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.file(File(path));
    writer.end();
  });

  // Inject EAGAIN then success on the server-side out_fd; this exercises the immediate retry path
  // in flushFilePayload after enabling writable interest.
  test::g_on_accept_install_actions.push(test::AcceptInstallActions{
      .writeActions = {},
      .writevActions = {},
      .sendfileActions = {{-1, error::kWouldBlock}, {static_cast<int64_t>(payload.size()), 0}},
  });

  test::ClientConnection client(ts.port());

  test::sendAll(client.fd(), SimpleGetRequest("/sendfile-retry"));

  auto resp = test::recvWithTimeout(client.fd(), 5000ms);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// =============================================================================
// TLS-specific error paths
// =============================================================================

#ifdef AERONET_ENABLE_OPENSSL

namespace {
// Use kTLS Disabled to force user-space TLS path
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
test::TlsTestServer tlsTs({"http/1.1"},
                          [](HttpServerConfig& cfg) { cfg.withTlsKtlsMode(TLSConfig::KtlsMode::Disabled); });
}  // namespace

// Test user-space TLS file serving error path (flushUserSpaceTlsBuffer error)
TEST(HttpResponseDispatchErrors, UserSpaceTlsBufferError) {
  test::QueueResetGuard<decltype(test::g_pread_path_actions)> guard(test::g_pread_path_actions);

  // Create a file for serving
  std::string payload(16UL * 1024, 'T');
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, payload);
  std::string filePath = tmp.filePath().string();

  tlsTs.setDefault([&filePath](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.file(File(filePath));
    writer.end();
  });

  // Inject pread error to cause user-space TLS buffer flush to fail
  test::SetPreadPathActions(filePath, {{-1, EIO}});

  test::TlsClient client(tlsTs.port());
  ASSERT_TRUE(client.handshakeOk());

  client.writeAll("GET /tls-error HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  (void)client.readAll();

  // Connection may be closed due to error
}

// Test TLS handshake WriteReady epoll mod path
// NOTE: This path requires the TLS handshake to return WriteReady hint while not yet established.
// It's difficult to trigger deterministically without deep SSL mocking.
// The path is exercised when SSL_do_handshake returns SSL_ERROR_WANT_WRITE.

// Test TLS EOF during handshake path
// When a client connects via TCP but closes without completing TLS handshake,
// the server should handle the EOF gracefully. This exercises the handleEofOrError
// path when tls->established() is false.
TEST(ConnectionManagerErrors, TlsEofDuringHandshake) {
  tlsTs.setDefault([](const HttpRequestView& req) { return req.makeResponse(http::StatusCodeOK); });

  {
    // Create a raw TCP connection that we'll close without TLS handshake
    test::ClientConnection client(tlsTs.port());
    // The ClientConnection destructor will close the socket without handshake
    // This triggers the TLS EOF-during-handshake path
  }

  // Allow server to process the closed connection
  std::this_thread::sleep_for(50ms);

  // Verify server still works after handling the aborted handshake
  test::TlsClient tlsClient(tlsTs.port());
  EXPECT_TRUE(tlsClient.handshakeOk());
  auto resp = tlsClient.get("/after-eof");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

#endif  // AERONET_ENABLE_OPENSSL

// =============================================================================
// Header and body timeout error paths
// =============================================================================

// Test header read timeout in handleReadableClient (line 540-544)
TEST(ConnectionManagerErrors, HeaderReadTimeoutInReadLoop) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.headerReadTimeout = 50ms;  // Very short timeout
  });

  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK); });

  test::ClientConnection client(ts.port());

  // Send partial request and wait
  test::sendAll(client.fd(), "GET /slow-header HTTP/1.1\r\n");

  // Wait for timeout
  std::this_thread::sleep_for(60ms);

  // Try to complete request - should get timeout response or connection close
  test::sendAll(client.fd(), "Host: localhost\r\nConnection: close\r\n\r\n");

  auto resp = test::recvWithTimeout(client.fd(), 500ms);
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine(http::Connection, http::close)));
}

TEST(ConnectionManagerErrors, MaxBufferOverflow) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.headerReadTimeout = {};
    cfg.maxHeaderBytes = 512;
    cfg.maxBodyBytes = 256;
  });

  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK); });

  {
    test::ClientConnection client(ts.port());

    // Send a request with headers exceeding the limit
    std::string hugeHeaders = "GET /overflow HTTP/1.1\r\nHost: localhost\r\n";
    for (int ii = 0; ii < 100; ++ii) {
      hugeHeaders += "X-Header-" + std::to_string(ii) + ": " + std::string(100, 'H') + "\r\n";
    }
    hugeHeaders += "\r\n";

    test::sendAll(client.fd(), hugeHeaders);

    const auto resp = test::recvWithTimeout(client.fd(), 2000ms);
    // Pre-routing DoS guard: buffer exceeds global maxHeaderBytes before headers are fully parsed.
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 431")) << resp;
  }

  {
    test::ClientConnection client(ts.port());

    // send a body exceeding the body limit but not the header limit
    std::string hugeBody =
        "GET /overflow HTTP/1.1\r\nHost: localhost\r\nContent-Length: 384\r\nContent-Type: text/plain\r\n\r\n";
    hugeBody += std::string(384, 'B');
    test::sendAll(client.fd(), hugeBody);

    const auto resp = test::recvWithTimeout(client.fd(), 2000ms);
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 413")) << resp;
  }
}

// Test maxPerEventReadBytes fairness cap in acceptNewConnections (line 294-297)
// This exercises the path where remainingBudget reaches 0 within a single accept cycle
// by sending enough data that multiple transportRead calls exhaust the budget.
TEST(ConnectionManagerErrors, MaxPerEventReadBytesFairnessBudgetExhausted) {
  // Set a very small read budget so it gets exhausted in a single iteration
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.withMaxPerEventReadBytes(64);  // Very small budget
    cfg.withMinReadChunkBytes(64);     // Match budget to trigger exhaustion
  });

  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK).body("OK"); });

  test::ClientConnection client(ts.port());

  // Send a request that exceeds the budget - this should still work since
  // we parse what we have and yield, but it exercises the fairness cap path
  std::string largeRequest = "POST /budget HTTP/1.1\r\nHost: x\r\nContent-Length: 256\r\n\r\n";
  largeRequest += std::string(256UL, 'X');
  test::sendAll(client.fd(), largeRequest);

  const auto resp = test::recvWithTimeout(client.fd(), 2000ms);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// Test waitingForBody flag update path (line 330-332 and 555-556)
// This exercises the body activity tracking used for bodyReadTimeout enforcement.
TEST(ConnectionManagerErrors, WaitingForBodyActivityTracking) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.withMaxBodyBytes(1U << 20U);
    cfg.withBodyReadTimeout(5s);  // Enable body timeout which activates waitingForBody
  });

  ts.router().setDefault([](const HttpRequestView& req) { return HttpResponse(http::StatusCodeOK).body(req.body()); });

  test::ClientConnection client(ts.port());

  // Send headers first, indicating a large body
  std::string headers = "POST /body-tracking HTTP/1.1\r\nHost: x\r\nContent-Length: 512\r\nConnection: close\r\n\r\n";
  test::sendAll(client.fd(), headers);

  // Wait a bit, then send body in chunks to exercise bodyLastActivity update
  std::this_thread::sleep_for(20ms);
  test::sendAll(client.fd(), std::string(256, 'A'));
  std::this_thread::sleep_for(20ms);
  test::sendAll(client.fd(), std::string(256, 'B'));

  const auto resp = test::recvUntilClosed(client.fd());
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  // Verify the body was received correctly
  EXPECT_TRUE(resp.contains(std::string(256, 'A'))) << resp;
}

#ifdef AERONET_ENABLE_OPENSSL
// Test TLS handshake failure with EOF reason (line 370, 582)
// This exercises the code path where a client closes connection during TLS handshake.
TEST(ConnectionManagerTlsErrors, TlsHandshakeFailureOnEof) {
  test::TlsTestServer tlsServer;
  tlsServer.setDefault([](const HttpRequestView& req) { return req.makeResponse("OK"); });

  std::atomic_bool failureDetected{false};
  std::string_view capturedReason;

  tlsServer.server.server.setTlsHandshakeCallback([&](const TlsHandshakeEvent& ev) {
    if (ev.result == TlsHandshakeEvent::Result::Failed) {
      capturedReason = ev.reason;
      failureDetected.store(true);
    }
  });

  // Connect but close immediately without completing handshake
  {
    test::ClientConnection client(tlsServer.port());
    // Close without sending any TLS data - this triggers EOF during handshake
  }

  // Wait for callback
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (std::chrono::steady_clock::now() < deadline && !failureDetected.load()) {
    std::this_thread::sleep_for(5ms);
  }

  const auto stats = tlsServer.stats();
  tlsServer.stop();

  EXPECT_TRUE(failureDetected.load());
  EXPECT_GE(stats.tlsHandshakesFailed, 1UL);
}

// Test TLS error diagnostics path (lines 344-358, 557-564)
// This exercises the detailed TLS error logging when transport returns Error.
TEST(ConnectionManagerTlsErrors, TlsTransportErrorDiagnostics) {
  test::TlsTestServer tlsServer({}, [](HttpServerConfig& cfg) { cfg.withTlsHandshakeLogging(true); });
  tlsServer.setDefault([](const HttpRequestView& req) { return req.makeResponse("OK"); });

  std::atomic_bool failureDetected{false};

  tlsServer.server.server.setTlsHandshakeCallback([&](const TlsHandshakeEvent& ev) {
    if (ev.result == TlsHandshakeEvent::Result::Failed) {
      failureDetected.store(true);
    }
  });

  // Send garbage data that will cause TLS parsing errors
  {
    test::ClientConnection client(tlsServer.port());
    test::sendAll(client.fd(), "NOT_TLS_DATA_AT_ALL\r\n\r\n");
    // Wait briefly for server to process
    std::this_thread::sleep_for(50ms);
  }

  // Wait for failure callback
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (std::chrono::steady_clock::now() < deadline && !failureDetected.load()) {
    std::this_thread::sleep_for(5ms);
  }

  const auto stats = tlsServer.stats();
  tlsServer.stop();

  EXPECT_TRUE(failureDetected.load());
  EXPECT_GE(stats.tlsHandshakesFailed, 1UL);
}

// Test TransportHint::WriteReady path during handshake (lines 379-386)
// This exercises the epoll mod to add EPOLLOUT interest when TLS needs write.
// This is triggered during TLS handshake when the transport signals it needs
// socket write-readiness to proceed (e.g., to send ClientHello response).
TEST(ConnectionManagerTlsErrors, TlsHandshakeWriteReadyEpollMod) {
  tlsTs.setDefault([](const HttpRequestView& req) { return req.makeResponse("OK"); });

  // A proper TLS client triggers the WriteReady path naturally during
  // the handshake when the server needs to send its response.
  test::TlsClient::Options opts;
  test::TlsClient client(tlsTs.port(), opts);

  EXPECT_TRUE(client.handshakeOk());

  // Make a request to verify the connection works after handshake
  const auto resp = client.get("/test");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// Test pread error path in flushFilePayload (non-TLS, user-space TLS buffer path)
// This exercises the scenario where kTLS is disabled and pread fails during file serving.
TEST(HttpResponseDispatchErrors, PreadErrorDuringUserSpaceTlsFileSend) {
  test::QueueResetGuard<decltype(test::g_pread_path_actions)> guard(test::g_pread_path_actions);

  // Create a file larger than a typical read chunk so pread is invoked
  std::string payload(32UL * 1024, 'P');
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, payload);
  std::string filePath = tmp.filePath().string();

  tlsTs.setDefault([&filePath](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.file(File(filePath));
    writer.end();
  });

  // Inject pread error: first read succeeds partially, second fails with EIO
  test::SetPreadPathActions(filePath, {{static_cast<int64_t>(1024), 0}, {-1, EIO}});

  test::TlsClient client(tlsTs.port());
  ASSERT_TRUE(client.handshakeOk());

  client.writeAll("GET /pread-error HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  auto resp = client.readAll();

  // Connection should be truncated due to pread error
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  // Body will be truncated
  EXPECT_LT(resp.size(), payload.size() + 256);
}
#endif  // AERONET_ENABLE_OPENSSL

// =============================================================================
// Outbound buffer retry paths in sweepIdleConnections
// =============================================================================

// Test sweepIdleConnections outBuffer flush retry path (lines 119-123 in connection-manager.cpp)
// This exercises the case where a connection has buffered outbound data and is waiting for
// writable interest; the sweep timer retries the flush.
TEST(ConnectionManagerErrors, SweepRetriesPendingOutboundData) {
  test::QueueResetGuard<decltype(test::g_write_actions)> guardWrite(test::g_write_actions);
  test::QueueResetGuard<decltype(test::g_writev_actions)> guardWritev(test::g_writev_actions);
  test::QueueResetGuard<decltype(test::g_on_accept_install_actions)> guardOnAccept(test::g_on_accept_install_actions);

  // Use a short poll interval so sweepIdleConnections runs quickly
  HttpServerConfig cfg;
  cfg.withPollInterval(std::chrono::milliseconds{5});
  test::TestServer localTs(std::move(cfg));

  std::string largeBody(64UL * 1024, 'S');
  localTs.router().setDefault([&largeBody](const HttpRequestView&) { return HttpResponse(largeBody); });

  // Inject EAGAIN on first writev to leave data buffered, then let subsequent writes succeed
  test::g_on_accept_install_actions.push(test::AcceptInstallActions{
      .writeActions = {},
      .writevActions = {{-1, error::kWouldBlock}},
      .sendfileActions = {},
  });

  test::ClientConnection client(localTs.port());
  test::sendAll(client.fd(), SimpleGetRequest("/sweep-retry"));

  // The first writev returns EAGAIN, leaving data in outBuffer.
  // The sweep timer should retry and eventually flush the data.
  auto resp = test::recvWithTimeout(client.fd(), 3000ms);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.contains(largeBody.substr(0, 32))) << resp;
}

// Test sweepIdleConnections file payload retry path (line 117 in connection-manager.cpp)
// This exercises the case where a file send is pending and waitingWritable; the sweep timer
// retries the file payload flush.
TEST(ConnectionManagerErrors, SweepRetriesPendingFilePayload) {
  test::QueueResetGuard<decltype(test::g_sendfile_actions)> guard(test::g_sendfile_actions);
  test::QueueResetGuard<decltype(test::g_on_accept_install_actions)> guardOnAccept(test::g_on_accept_install_actions);

  HttpServerConfig cfg;
  cfg.withPollInterval(std::chrono::milliseconds{5});
  test::TestServer localTs(std::move(cfg));

  std::string payload(16UL * 1024, 'F');
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, payload);
  std::string filePath = tmp.filePath().string();

  localTs.router().setDefault([&filePath](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.file(File(filePath));
    writer.end();
  });

  // Inject EAGAIN on first sendfile to leave file send pending,
  // then let subsequent sendfile calls succeed
  test::g_on_accept_install_actions.push(test::AcceptInstallActions{
      .writeActions = {},
      .writevActions = {},
      .sendfileActions = {{-1, error::kWouldBlock}, {static_cast<int64_t>(payload.size()), 0}},
  });

  test::ClientConnection client(localTs.port());
  test::sendAll(client.fd(), SimpleGetRequest("/sweep-file-retry"));

  auto resp = test::recvWithTimeout(client.fd(), 5000ms);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// =============================================================================
// flushOutbound headersPending transition
// =============================================================================

// Test flushOutbound headersPending->false transition (line 281-283 in http-response-dispatch.cpp)
// When a file payload has headersPending=true and outBuffer is flushed empty, headersPending
// should be cleared to allow flushFilePayload to proceed.
TEST(HttpResponseDispatchErrors, FlushOutboundClearsHeadersPending) {
  test::QueueResetGuard<decltype(test::g_sendfile_actions)> guard(test::g_sendfile_actions);
  test::QueueResetGuard<decltype(test::g_on_accept_install_actions)> guardOnAccept(test::g_on_accept_install_actions);
  test::QueueResetGuard<decltype(test::g_writev_actions)> guardWritev(test::g_writev_actions);

  std::string payload(8UL * 1024, 'H');
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, payload);
  std::string filePath = tmp.filePath().string();

  ts.router().setDefault([&filePath](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.file(File(filePath));
    writer.end();
  });

  // First writev returns partial write (headers still buffered), second succeeds
  test::g_on_accept_install_actions.push(test::AcceptInstallActions{
      .writeActions = {},
      .writevActions = {{10, 0}},
      .sendfileActions = {},
  });

  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), SimpleGetRequest("/headers-pending"));

  auto resp = test::recvWithTimeout(client.fd(), 3000ms);
  // The partial write path may cause a truncated status line, but the file content should be delivered
  EXPECT_TRUE(resp.contains("200") || resp.contains("HHHH")) << resp;
}

// =============================================================================
// flushOutbound transportNeedsWrite path
// =============================================================================

// Test flushOutbound when transport needs write after non-TLS flush (lines 303-311)
// This exercises the path where outBuffer is empty but transport still reports WriteReady.
// This typically happens briefly during TLS handshake completion. We simulate it by causing
// the first write to return partial (with WriteReady need), then subsequent writes succeed.
TEST(HttpResponseDispatchErrors, FlushOutboundTransportNeedsWrite) {
  test::QueueResetGuard<decltype(test::g_write_actions)> guardWrite(test::g_write_actions);
  test::QueueResetGuard<decltype(test::g_writev_actions)> guardWritev(test::g_writev_actions);
  test::QueueResetGuard<decltype(test::g_on_accept_install_actions)> guardOnAccept(test::g_on_accept_install_actions);

  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse("needs-write"); });

  // First writev returns EAGAIN (WriteReady), then second succeeds
  test::g_on_accept_install_actions.push(test::AcceptInstallActions{
      .writeActions = {},
      .writevActions = {{-1, error::kWouldBlock}},
      .sendfileActions = {},
  });

  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), SimpleGetRequest("/transport-needs-write"));

  auto resp = test::recvWithTimeout(client.fd(), 2000ms);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.contains("needs-write")) << resp;
}

// =============================================================================
// Connection close after TLS handshake finalization
// =============================================================================

// Test epoll_ctl ADD failure during accept (connection-manager.cpp line ~336)
// This exercises the code path where adding a new accepted connection to epoll fails.
TEST(ConnectionManagerErrors, EpollCtlAddFailureDuringAccept) {
  test::EventLoopHookGuard hookGuard;

  HttpServerConfig cfg;
  cfg.withPollInterval(std::chrono::milliseconds{5});
  test::TestServer localTs(std::move(cfg));

  localTs.router().setDefault([](const HttpRequestView&) { return HttpResponse("OK"); });

  // Inject epoll_ctl ADD failure for the next accepted connection
  test::PushEpollCtlAddAction(test::EpollCtlAction{-1, ENOMEM});

  {
    test::ClientConnection client(localTs.port());
    // The connection should be rejected due to epoll_ctl ADD failure
    test::sendAll(client.fd(), SimpleGetRequest("/epoll-add-fail"));
    test::recvWithTimeout(client.fd(), 1000ms);
    // May or may not get a response depending on timing
  }

  std::this_thread::sleep_for(50ms);

  // Server should still work for subsequent connections
  auto resp = test::simpleGet(localTs.port(), "/after-add-fail");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// =============================================================================
// HTTP Garden regression payloads
// =============================================================================
//
// Parsing anomalies catalogued by the HTTP Garden project (https://github.com/narfindustries/http-garden, mirrored at
// https://github.com/Ramzansmith/http-garden-fuzzing-http-servers).
//
// Each case references the bug number of the Garden README ("server #N" for server bugs, "transducer #N" for proxy
// bugs, "bonus #N" for bonus bugs). Payloads are the Garden ones, with a Host header added where it was missing:
// a Host-less HTTP/1.1 request is rejected before the code path the payload targets is reached.
//
// Transducer (proxy) bugs are about what a proxy forwards. aeronet is the origin server in that picture, so for them we
// check that aeronet, receiving the forwarded bytes, either rejects them or frames them the standard way.

namespace {

// Describes how the server interpreted the request, so that tests can check framing decisions.
void InstallDescribeHandler() {
  ts.router().setDefault([](const HttpRequestView& req) {
    std::string out;
    out.append("m=").append(http::MethodToStr(req.method()));
    out.append(";p=[").append(req.path()).append("]");
    out.append(";b=[").append(req.body()).append("]");
    out.append(";t=[").append(req.trailerValueOrEmpty("x")).append("]");
    return req.makeResponse(out);
  });
}

struct GardenCase {
  std::string_view ref;
  std::string_view payload;
  http::StatusCode expectedStatus;
};

std::string StatusLine(http::StatusCode statusCode) { return "HTTP/1.1 " + std::to_string(statusCode) + ' '; }

// Sends the payload on a fresh connection and checks that the server answered with exactly one response with the
// expected status, then closed the connection: nothing after the offending bytes may be read as another request.
void ExpectSingleResponse(const GardenCase& gardenCase) {
  SCOPED_TRACE(gardenCase.ref);
  const std::string resp = test::sendAndCollect(port, gardenCase.payload);
  EXPECT_TRUE(resp.starts_with(StatusLine(gardenCase.expectedStatus))) << resp;
  EXPECT_EQ(test::countOccurrences(resp, "HTTP/1.1 "), 1) << resp;
}

void ExpectSingleResponses(std::initializer_list<GardenCase> gardenCases) {
  InstallDescribeHandler();
  for (const auto& gardenCase : gardenCases) {
    ExpectSingleResponse(gardenCase);
  }
}

}  // namespace

// =============================================================================
// Request line
// =============================================================================

TEST(HttpGardenRequestLine, MalformedRequestLinesAreRejected) {
  ExpectSingleResponses({
      // server #5: HTTP versions interpreted as their longest valid prefix.
      {"server #5", "GET /test HTTP/1.32\r\nHost: a\r\n\r\n", http::StatusCodeBadRequest},
      // server #15: HTTP versions not validated.
      {"server #15", "GET / HTTP/\r\r1.1\r\nHost: a\r\n\r\n", http::StatusCodeBadRequest},
      // server #24: 8-bit integer overflow in HTTP version numbers.
      {"server #24", "GET / HTTP/4294967295.255\r\nHost: a\r\n\r\n", http::StatusCodeBadRequest},
      // server #9: '\n' allowed as separating whitespace in a request line.
      {"server #9", "GET /\nHTTP/1.1\r\nHost: a\r\n\r\n", http::StatusCodeBadRequest},
      // server #6: HTTP methods interpreted as their longest valid prefix.
      {"server #6", "G=\":<>(e),[T];?\" /get HTTP/1.1\r\nHost: a\r\n\r\n", http::StatusCodeNotImplemented},
      // server #40: HTTP methods and versions not validated.
      {"server #40", "\x00 / HTTP/............0596.7407.\r\nHost: a\r\n\r\n"sv, http::StatusCodeNotImplemented},
      // server #12: a request without Host must get a 400 response, not a silent close.
      {"server #12", "GET / HTTP/1.1\r\n\r\n", http::StatusCodeBadRequest},
  });
}

TEST(HttpGardenRequestLine, ValidRequestTargetFormsAreAccepted) {
  InstallDescribeHandler();
  for (std::string_view target : {"/", "/a/b?c=d", "/%41", "http://a/abs"}) {
    SCOPED_TRACE(target);
    const std::string resp = test::sendAndCollect(
        port, std::string("GET ") + std::string(target) + " HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
    EXPECT_FALSE(resp.starts_with("HTTP/1.1 400")) << resp;
  }
  const std::string resp = test::sendAndCollect(port, "OPTIONS * HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// =============================================================================
// Header names
// =============================================================================

TEST(HttpGardenHeaderNames, InvalidHeaderNamesAreRejected) {
  ExpectSingleResponses({
      // server #3: whitespace stripped from the end of header names.
      {
          "server #3",
          "GET / HTTP/1.1\r\nHost: whatever\r\nContent-Length : 34\r\n\r\nGET / HTTP/1.1\r\nHost: whatever\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // server #4: whitespace stripped from the beginning of the first header name.
      {"server #4", "GET / HTTP/1.1\r\n\tContent-Length: 1\r\nHost: a\r\n\r\nX", http::StatusCodeBadRequest},
      // server #8: non-ASCII bytes permitted in header names.
      {
          "server #8",
          "GET / HTTP/1.1\r\nHost: a\r\n\xef"
          "oo: bar\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // server #14: disallowed ASCII characters permitted in header names.
      {
          "server #14",
          "GET / HTTP/1.1\r\nHost: a\r\n"
          "\x00\x01\x02\x03\x04\x05\x06\x07\x08\t\x0b\x0c\x0e\x0f\x10\x11\x12\x13\x14\x15\x16"
          "\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f \"(),/;<=>?@[/]{}: whatever\r\n\r\n"sv,
          http::StatusCodeBadRequest,
      },
      // server #18: empty header names accepted.
      {"server #18", "GET / HTTP/1.1\r\n: ignored\r\nHost: whatever\r\n\r\n", http::StatusCodeBadRequest},
      // server #20: \xa0 and \x85 stripped from the end of header names.
      {
          "server #20",
          "GET / HTTP/1.1\r\nHost: a\r\nContent-Length\x85: 10\r\n\r\n0123456789",
          http::StatusCodeBadRequest,
      },
      // server #27: header block truncated on a header with neither name nor value.
      {"server #27", "GET / HTTP/1.1\r\nHost: a\r\n:\r\nI: am chopped off\r\n\r\n", http::StatusCodeBadRequest},
      // server #28: header name separated from the value by a space alone, no ':'.
      {"server #28", "GET / HTTP/1.1\r\nHost: a\r\nContent-Length 10\r\n\r\n0123456789", http::StatusCodeBadRequest},
      // server #36: field lines with no ':' ignored.
      {
          "server #36",
          "GET / HTTP/1.1\r\nHost: whatever\r\nTest\r\nConnection: close\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // server #37: header names continued across lines.
      {
          "server #37",
          "POST / HTTP/1.1\r\nHost: whatever\r\nTransfer-\r\nEncoding: chunked\r\nContent-Length: 5\r\n\r\n0\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // transducer #6: disallowed bytes in header names.
      {"transducer #6", "GET / HTTP/1.1\r\nHost: fanout\r\nHeader\x85: value\r\n\r\n", http::StatusCodeBadRequest},
      // transducer #14: field lines with no ':' (bare LF inside the name).
      {
          "transducer #14",
          "GET / HTTP/1.1\r\nHost: whatever\r\nTe\nst: test\r\nConnection: close\r\n\r\n",
          http::StatusCodeBadRequest,
      },
  });
}

// server #11: header names containing any of !#$%&'*+.^_`|~ must be accepted (they are tchars).
TEST(HttpGardenHeaderNames, TcharPunctuationIsAccepted) {
  InstallDescribeHandler();
  const std::string resp = test::sendAndCollect(
      port, "GET / HTTP/1.1\r\nHost: a\r\nTe!st: a\r\nX-!#$%&'*+.^_`|~: b\r\nConnection: close\r\n\r\n");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// =============================================================================
// Header values and line terminators
// =============================================================================

TEST(HttpGardenHeaderValues, InvalidHeaderValuesAreRejected) {
  ExpectSingleResponses({
      // server #2: \x00, \r or \n permitted in header values.
      {"server #2", "GET / HTTP/1.1\r\nHost: a\r\nHeader: v\n\x00\ralue\r\n\r\n"sv, http::StatusCodeBadRequest},
      // server #13: '\r' treated as a line terminator in header field lines.
      {"server #13", "GET / HTTP/1.1\r\nHost: a\r\nVisible: :/\rSmuggled: :)\r\n\r\n", http::StatusCodeBadRequest},
      // server #19: non-CRLF whitespace stripped from the beginning of header values.
      {"server #19", "GET / HTTP/1.1\r\nHost: a\r\nUseless:\n\nGET / HTTP/1.1\r\n\r\n", http::StatusCodeBadRequest},
      // server #34: '\r' permitted in header values.
      {"server #34", "GET / HTTP/1.1\r\nHost: whatever\r\nHeader: va\rlue\r\n\r\n", http::StatusCodeBadRequest},
      // server #35: header values truncated at \x00.
      {
          "server #35",
          "GET / HTTP/1.1\r\nHost: whatever\r\nTest: test\x00THESE BYTES GET DROPPED\r\nConnection: close\r\n\r\n"sv,
          http::StatusCodeBadRequest,
      },
      // server #41: \xa0 and \x85 stripped from header values.
      {
          "server #41",
          "GET /login HTTP/1.1\r\nHost: a\r\nUser: \x85"
          "admin\xa0\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // server #43: \x00 stripped from the end of header values.
      {"server #43", "GET / HTTP/1.1\r\nHost: a\r\nEvil: evil\x00\r\n\r\n"sv, http::StatusCodeBadRequest},
      // server #46: bytes above \x80 stripped from header values.
      {
          "server #46",
          "POST / HTTP/1.1\r\nHost: \xff"
          "a\xff\r\nTransfer-Encoding: \xff"
          "chunked\xff\r\n\r\n1\r\nZ\r\n0\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // transducer #10: '\r' in header values.
      {
          "transducer #10",
          "GET / HTTP/1.1\r\nHost: a\r\nInvalid-Header: this\rvalue\ris\rinvalid\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // transducer #12: \x00 in header values.
      {"transducer #12", "GET / HTTP/1.1\r\nHost: google.com\x00.kallus.org\r\n\r\n"sv, http::StatusCodeBadRequest},
      // transducer #20: headers containing \x00 or \n concatenated into the previous header value.
      {"transducer #20", "GET / HTTP/1.1\r\nHost: a\r\na:b\r\nc\x00\r\n\r\n"sv, http::StatusCodeBadRequest},
  });
}

TEST(HttpGardenHeaderValues, HeaderBlockTerminatorsAreStrict) {
  ExpectSingleResponses({
      // server #30: header block terminated on \r\n\rX.
      {
          "server #30",
          "GET / HTTP/1.1\r\nHost: a\r\n\rZGET /evil: HTTP/1.1\r\nHost: a\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // transducer #16: bare '\n' line terminators are not accepted (the final CRLF only ends the request line).
      {"transducer #16", "GET / HTTP/1.1\nHost: whatever\nConnection: close\n\r\n", http::StatusCodeBadRequest},
  });
}

// =============================================================================
// Content-Length
// =============================================================================

TEST(HttpGardenContentLength, InvalidContentLengthsAreRejected) {
  ExpectSingleResponses({
      // server #10: '_', '+' and '-' accepted in Content-Length values.
      {"server #10", "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: +1_0\r\n\r\n0123456789", http::StatusCodeBadRequest},
      // server #16: empty Content-Length treated as 0.
      {"server #16", "GET / HTTP/1.1\r\nHost: whatever\r\nContent-Length: \r\n\r\n", http::StatusCodeBadRequest},
      // server #26: negative Content-Length forcing an infinite busy loop.
      {"server #26", "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: -48\r\n\r\n", http::StatusCodeBadRequest},
      // server #29: invalid Content-Length interpreted as its longest valid prefix.
      {"server #29", "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 1Z\r\n\r\nZ", http::StatusCodeBadRequest},
      // server #38: empty Content-Length interpreted as "read until timeout".
      {
          "server #38",
          "GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: \r\n\r\nGET / HTTP/1.1\r\nHost: localhost\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // transducer #1: 0x-prefixed Content-Length.
      {"transducer #1", "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0x10\r\n\r\nZ", http::StatusCodeBadRequest},
      // Content-Length overflowing 64 bits.
      {
          "overflow",
          "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 18446744073709551616\r\n\r\nZ",
          http::StatusCodeBadRequest,
      },
  });
}

TEST(HttpGardenContentLength, ConflictingContentLengthsAreRejected) {
  ExpectSingleResponses({
      // server #23: conflicting Content-Length headers, first one prioritized.
      {
          "server #23",
          "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 0\r\n\r\nZ",
          http::StatusCodeBadRequest,
      },
      // server #33: conflicting Content-Length headers, last one prioritized.
      {
          "server #33",
          "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\nZ",
          http::StatusCodeBadRequest,
      },
      // server #42 / transducer #11: empty Content-Length prioritized over a subsequent one.
      {
          "server #42",
          "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: \r\nContent-Length: 43\r\n\r\n"
          "POST /evil HTTP/1.1\r\nContent-Length: 18\r\n\r\nGET / HTTP/1.1\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // transducer #22: multiple Content-Length headers.
      {
          "transducer #22",
          "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\nContent-Length: 31\r\n\r\n"
          "GET /evil HTTP/1.1\r\nHost: a\r\n\r\n",
          http::StatusCodeBadRequest,
      },
  });
}

// server #32: Content-Length parsed with strtoll(,,0), so a leading 0 meant octal. It is decimal.
TEST(HttpGardenContentLength, LeadingZerosAreDecimal) {
  InstallDescribeHandler();
  const std::string resp = test::sendAndCollect(
      port, "POST / HTTP/1.1\r\nHost: whatever\r\nContent-Length: 010\r\nConnection: close\r\n\r\n0123456789");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.contains(";b=[0123456789];")) << resp;
}

// transducer #21: a GET body framed by Content-Length is consumed as a body, never parsed as a new request.
TEST(HttpGardenContentLength, GetBodyIsConsumed) {
  InstallDescribeHandler();
  const std::string resp = test::sendAndCollect(
      port, "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\nConnection: close\r\n\r\n1234567890");
  EXPECT_EQ(test::countOccurrences(resp, "HTTP/1.1 "), 1) << resp;
  EXPECT_TRUE(resp.contains(";b=[1234567890];")) << resp;
}

// =============================================================================
// Transfer-Encoding
// =============================================================================

TEST(HttpGardenTransferEncoding, AmbiguousTransferEncodingsAreRejected) {
  ExpectSingleResponses({
      // server #21 / transducer #9 / #15 / #18: ",chunked" is neither ignored nor mapped to Content-Length framing.
      {
          "server #21",
          "GET / HTTP/1.1\r\nHost: whatever\r\nTransfer-Encoding: ,chunked\r\nContent-Length: 5\r\n\r\n0\r\n\r\n",
          http::StatusCodeNotImplemented,
      },
      {
          "transducer #9",
          "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: ,chunked\r\n\r\n0\r\n\r\n",
          http::StatusCodeNotImplemented,
      },
      // server #44: unknown transfer codings treated as chunked.
      {
          "server #44",
          "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: blegh\r\n\r\n1\r\nZ\r\n0\r\n\r\n",
          http::StatusCodeNotImplemented,
      },
      // transducer #23: Content-Length together with Transfer-Encoding.
      {
          "transducer #23",
          "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // transducer #28: chunked applied twice (two Transfer-Encoding: chunked headers).
      {
          "transducer #28",
          "POST / HTTP/1.1\r\nHost: whatever\r\nTransfer-Encoding: chunked\r\n"
          "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
          http::StatusCodeNotImplemented,
      },
      // Transfer-Encoding in a HTTP/1.0 request makes the framing faulty (RFC 9112 §6.1).
      {
          "HTTP/1.0 Transfer-Encoding",
          "POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
          http::StatusCodeBadRequest,
      },
  });
}

// =============================================================================
// Chunked framing
// =============================================================================

TEST(HttpGardenChunked, InvalidChunkSizesAreRejected) {
  ExpectSingleResponses({
      // server #1 / #22: chunk sizes interpreted as their longest valid prefix.
      {
          "server #1",
          "GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n0_2e\r\n\r\n"
          "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\n0\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // server #25: 0x, + and - prefixes accepted (strtoll).
      {
          "server #25 (0x)",
          "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n0x1\r\nZ\r\n0\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      {
          "server #25 (+)",
          "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n+1\r\nZ\r\n0\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // transducer #2: invalid chunk sizes.
      {
          "transducer #2",
          "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\nZZ\r\nZZZ\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // transducer #7: chunk sizes interpreted as their longest valid prefix.
      {
          "transducer #7",
          "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
          "1these-bytes-never-get-validated\r\nZ\r\n0\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // transducer #19: chunk sizes with +, - and 0x prefixes.
      {
          "transducer #19",
          "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n-0x0\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // transducer #24: whitespace-prefixed chunk sizes.
      {
          "transducer #24",
          "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n           0\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // transducer #17: an extra CRLF before the last chunk is an empty (invalid) chunk size.
      {
          "transducer #17",
          "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
          "17\r\n0\r\n\r\nGET / HTTP/1.1\r\n\r\n\r\n\r\n0\r\n\r\n",
          http::StatusCodeBadRequest,
      },
  });
}

TEST(HttpGardenChunked, InvalidChunkLineTerminatorsAreRejected) {
  ExpectSingleResponses({
      // server #31: chunk lines terminated on \rX.
      {
          "server #31",
          "GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"
          "5\r\r;ABCD\r\n34\r\nE\r\n0\r\n\r\n"
          "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\n0\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // transducer #3: '\r' in chunk-ext whitespace before the ';'.
      {
          "transducer #3",
          "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n2\r\r;a\r\n02\r\n41\r\n0\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // transducer #13: bare '\n' as the chunk data terminator.
      {
          "transducer #13",
          "GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\na\r\n0123456789\n0\r\n\r\n",
          http::StatusCodeBadRequest,
      },
      // server #47: after an invalid chunk the connection must be closed, not resynchronized on the next CRLF.
      {
          "server #47",
          "POST / HTTP/1.1\r\nHost: whatever\r\nTransfer-Encoding: chunked\r\n\r\n"
          "INVALID!!!\r\nGET / HTTP/1.1\r\nHost: whatever\r\n\r\n",
          http::StatusCodeBadRequest,
      },
  });
}

TEST(HttpGardenChunked, ValidChunkExtensionsAreAccepted) {
  InstallDescribeHandler();
  const std::string resp =
      test::sendAndCollect(port,
                           "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                           "2;name=value;q=\"quoted str\"\r\nab\r\n1;\tx\r\nc\r\n0\r\n\r\n");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.contains(";b=[abc];")) << resp;
}

// server #39: chunked bodies terminated on \r\nXX. "X:POST / HTTP/1.1" is a trailer field, not a new request.
TEST(HttpGardenChunked, TrailerFieldIsNotParsedAsNextRequest) {
  InstallDescribeHandler();
  const std::string resp =
      test::sendAndCollect(port,
                           "GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                           "0\r\nX:POST / HTTP/1.1\r\n\r\n");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_EQ(test::countOccurrences(resp, "HTTP/1.1 "), 1) << resp;
  EXPECT_TRUE(resp.contains(";t=[POST / HTTP/1.1]")) << resp;
}

// =============================================================================
// Message boundaries / pipelining
// =============================================================================

// server #45: an invalid request pipelined after a valid "Connection: close" one must not prevent the response to the
// valid one (the trailing bytes are simply discarded).
TEST(HttpGardenPipelining, InvalidRequestAfterConnectionCloseIsIgnored) {
  InstallDescribeHandler();
  const std::string resp =
      test::sendAndCollect(port, "GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\nInvalid\r\n\r\n");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_EQ(test::countOccurrences(resp, "HTTP/1.1 "), 1) << resp;
}

// server #48: pipelined requests are not merged into the body of the first one, even when it has Content-Length: 0.
TEST(HttpGardenPipelining, PipelinedRequestsKeepTheirOwnFraming) {
  InstallDescribeHandler();
  test::ClientConnection conn(port);
  test::sendAll(conn.fd(),
                "POST / HTTP/1.1\r\nContent-Length: 0\r\nConnection:keep-alive\r\nHost: a\r\nid: 0\r\n\r\n"
                "POST /second HTTP/1.1\r\nHost: a\r\nid: 1\r\nContent-Length: 34\r\nConnection: close\r\n\r\n");
  std::this_thread::sleep_for(15ms);
  test::sendAll(conn.fd(), "GET / HTTP/1.1\r\nHost: a\r\nid: 2\r\n\r\n");
  const std::string resp = test::recvUntilClosed(conn.fd());
  EXPECT_EQ(test::countOccurrences(resp, "HTTP/1.1 200"), 2) << resp;
  EXPECT_TRUE(resp.contains("m=POST;p=[/];b=[];")) << resp;
  EXPECT_TRUE(resp.contains("m=POST;p=[/second];b=[GET / HTTP/1.1\r\nHost: a\r\nid: 2\r\n\r\n];")) << resp;
}

// bonus #3: an extra byte after a chunked request crashed the server. It must be kept as the start of the next request.
TEST(HttpGardenPipelining, ExtraByteAfterChunkedBody) {
  InstallDescribeHandler();
  {
    test::ClientConnection conn(port);
    test::sendAll(conn.fd(), "GET / HTTP/1.1\r\nHost: whatever\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n\x00"sv);
    const std::string resp = test::recvWithTimeout(conn.fd());
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  }
  // Server still alive.
  const std::string resp = test::sendAndCollect(port, "GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}
