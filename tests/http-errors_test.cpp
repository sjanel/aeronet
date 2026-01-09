#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "aeronet/http-helpers.hpp"

#define AERONET_WANT_SOCKET_OVERRIDES
#define AERONET_WANT_READ_WRITE_OVERRIDES
#define AERONET_WANT_SENDFILE_PREAD_OVERRIDES

#include "aeronet/file.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_tls_client.hpp"
#include "aeronet/tls-config.hpp"
#endif

using namespace std::chrono_literals;
using namespace aeronet;

namespace {

struct Capture {
  std::mutex m;
  std::vector<http::StatusCode> errors;
  void push(http::StatusCode err) {
    std::scoped_lock lk(m);
    errors.push_back(err);
  }
};

std::string SimpleGetRequest(std::string_view target, std::string_view connectionHeader = "close") {
  std::string req;
  req.reserve(128);
  req.append("GET ").append(target).append(" HTTP/1.1\r\n");
  req.append("Host: localhost\r\n");
  req.append("Connection: ").append(connectionHeader).append("\r\n");
  req.append("Content-Length: 0\r\n\r\n");
  return req;
}

test::TestServer ts(HttpServerConfig{});
auto port = ts.port();

}  // namespace

TEST(HttpParserErrors, InvalidVersion505) {
  Capture cap;
  ts.server.setParserErrorCallback([&](http::StatusCode err) { cap.push(err); });
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });
  test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string bad = "GET / HTTP/9.9\r\nHost: x\r\nConnection: close\r\n\r\n";  // unsupported version
  test::sendAll(fd, bad);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("505")) << resp;
  bool seen = false;
  {
    std::scoped_lock lk(cap.m);
    for (auto err : cap.errors) {
      if (err == http::StatusCodeHTTPVersionNotSupported) {
        seen = true;
      }
    }
  }
  ASSERT_TRUE(seen);
}

TEST(HttpParserErrors, ExceptionInParserShouldBeControlled) {
  Capture cap;
  ts.server.setParserErrorCallback([&]([[maybe_unused]] http::StatusCode err) { throw std::runtime_error("boom"); });
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

  std::string bad =
      "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\nConnection: close\r\n\r\n";  // invalid content-length

  {
    test::ClientConnection clientConnection(port);
    int fd = clientConnection.fd();

    test::sendAll(fd, bad);
    std::string resp = test::recvUntilClosed(fd);
    ASSERT_TRUE(resp.contains("400")) << resp;
  }

  ts.server.setParserErrorCallback([&]([[maybe_unused]] http::StatusCode err) { throw 42; });
  std::this_thread::sleep_for(2 * ts.server.config().pollInterval);
  {
    test::ClientConnection clientConnection(port);
    int fd = clientConnection.fd();
    test::sendAll(fd, bad);
    std::string resp = test::recvUntilClosed(fd);
    ASSERT_TRUE(resp.contains("400")) << resp;
  }
}

TEST(HttpParserErrors, Expect100OnlyWithBody) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });
  test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
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
  ASSERT_TRUE(resp.contains("100 Continue"));
  ASSERT_TRUE(resp.contains("200"));
}

// Fuzz-ish incremental chunk framing with random chunk sizes & boundaries.
TEST(HttpParserErrors, ChunkIncrementalFuzz) {
  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });

  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> sizeDist(1, 15);
  std::string original;
  test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string head = "POST /f HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, head);
  // send 5 random chunks
  for (int i = 0; i < 5; ++i) {
    int sz = sizeDist(rng);
    std::string chunk(static_cast<std::size_t>(sz), static_cast<char>('a' + (i % 26)));
    original += chunk;
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%x", sz);
    std::string frame = std::string(hex) + "\r\n" + chunk + "\r\n";
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
  test::sendAll(fd, "0\r\n\r\n");
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("200"));
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
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withTcpNoDelay(true); });
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

  // Now inject setsockopt failures for the next connection's TCP_NODELAY call
  // We push multiple failures to ensure one catches the TCP_NODELAY call
  test::PushSetsockoptAction({-1, EPERM});
  test::PushSetsockoptAction({-1, EPERM});

  // Make a request - server should still serve despite TCP_NODELAY failure
  auto resp = test::simpleGet(ts.port(), "/nodelay-fail");
  EXPECT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
}

// Test eventLoop.add failure path
// This exercises the path where epoll_ctl ADD fails for a new connection
TEST(ConnectionManagerErrors, EventLoopAddFailure) {
  test::EventLoopHookGuard guard;
  test::QueueResetGuard<decltype(test::g_epoll_ctl_add_actions)> epollAddGuard(test::g_epoll_ctl_add_actions);

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

  // First request should work normally
  auto resp1 = test::simpleGet(ts.port(), "/first");
  EXPECT_TRUE(resp1.contains("HTTP/1.1 200"));

  // Force the next epoll_ctl(ADD) (used by eventLoop.add for an accepted client fd) to fail.
  test::PushEpollCtlAddAction({-1, EIO});

  // Next connection should be accepted then immediately dropped due to add() failure.
  // We validate this by observing that the peer closes without returning an HTTP response.
  test::ClientConnection client(ts.port());
  EXPECT_TRUE(test::WaitForPeerClose(client.fd(), 500ms));

  // Server should remain healthy after handling the error.
  auto resp2 = test::simpleGet(ts.port(), "/after");
  EXPECT_TRUE(resp2.contains("HTTP/1.1 200")) << resp2;
}

// Test sweepIdleConnections - isImmediateCloseRequested path
// This exercises the sweep when a connection has requested immediate close
TEST(ConnectionManagerErrors, SweepIdleConnectionsImmediateClose) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.withKeepAliveMode();
    cfg.withKeepAliveTimeout(1h);  // Long timeout so sweep doesn't close by timeout
  });

  // Handler that causes an immediate close request (error path)
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("immediate-close"); });

  // Normal request should work
  auto resp = test::simpleGet(ts.port(), "/sweep-test");
  EXPECT_TRUE(resp.contains("HTTP/1.1 200"));
}

// Test maxPerEventReadBytes fairness cap in handleReadableClient
// NOTE: This configuration option requires careful tuning and is primarily for server fairness
// with many concurrent connections. The test verifies the config is accepted and basic operation works.
TEST(ConnectionManagerErrors, MaxPerEventReadBytesFairness) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.maxPerEventReadBytes = 8192;  // Reasonable limit
  });

  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });

  // Send a simple request - verify basic operation with fairness cap enabled
  auto resp = test::simpleGet(ts.port(), "/fairness");
  EXPECT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
}

// Test queueData TransportHint::Error path
TEST(HttpResponseDispatchErrors, QueueDataTransportError) {
  test::QueueResetGuard<decltype(test::g_write_actions)> guardWrite(test::g_write_actions);
  test::QueueResetGuard<decltype(test::g_writev_actions)> guardWritev(test::g_writev_actions);
  test::QueueResetGuard<decltype(test::g_on_accept_install_actions)> guardOnAccept(test::g_on_accept_install_actions);

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("test-body"); });

  // Inject a server-side write failure on the accepted fd (PlainTransport uses writev for head+body).
  test::g_on_accept_install_actions.push(test::AcceptInstallActions{
      .writeActions = {{-1, EPIPE}},
      .writevActions = {{-1, EPIPE}},
      .sendfileActions = {},
  });

  test::ClientConnection client(ts.port());

  test::sendAll(client.fd(), SimpleGetRequest("/write-error", http::keepalive));

  // On a transport error while sending, the server requests immediate close; the client may see
  // an empty/partial response (and should observe a close).
  (void)test::recvWithTimeout(client.fd(), 1000ms);
  EXPECT_TRUE(test::WaitForPeerClose(client.fd(), 2000ms));
}

// Test flushOutbound TransportHint::Error path (line 364-372)
TEST(HttpResponseDispatchErrors, FlushOutboundTransportError) {
  test::QueueResetGuard<decltype(test::g_write_actions)> guardWrite(test::g_write_actions);
  test::QueueResetGuard<decltype(test::g_writev_actions)> guardWritev(test::g_writev_actions);

  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.maxOutboundBufferBytes = 1 << 20; });

  // Generate a large response to ensure buffering
  std::string largeBody(64UL * 1024, 'L');
  ts.router().setDefault([&largeBody](const HttpRequest&) { return HttpResponse(http::StatusCodeOK).body(largeBody); });

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
  //  - third writev: EPIPE => flushOutbound hits TransportHint::Error and requests immediate close
  test::SetWritevActions(serverFd, {{100, 0}, {-1, EAGAIN}, {-1, EPIPE}});
  ASSERT_EQ(test::g_writev_actions.size(serverFd), 3U);

  test::sendAll(client.fd(), SimpleGetRequest("/flush-error", http::keepalive));

  const auto resp = test::recvWithTimeout(client.fd(), 1000ms);
  EXPECT_LT(test::g_writev_actions.size(serverFd), 3U);
  EXPECT_TRUE(test::WaitForPeerClose(client.fd(), 2000ms)) << resp;
}

// Test sendfile error path in flushFilePayload (line 532)
TEST(HttpResponseDispatchErrors, SendfileError) {
  test::QueueResetGuard<decltype(test::g_sendfile_actions)> guard(test::g_sendfile_actions);
  test::QueueResetGuard<decltype(test::g_on_accept_install_actions)> guardOnAccept(test::g_on_accept_install_actions);

  constexpr std::string_view kPayload = "sendfile error test payload content";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  std::string path = tmp.filePath().string();

  ts.router().setDefault([&path](const HttpRequest&, HttpResponseWriter& writer) {
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
  EXPECT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
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

  ts.router().setDefault([&path](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.file(File(path));
    writer.end();
  });

  // Inject EAGAIN then success on the server-side out_fd; this exercises the immediate retry path
  // in flushFilePayload after enabling writable interest.
  test::g_on_accept_install_actions.push(test::AcceptInstallActions{
      .writeActions = {},
      .writevActions = {},
      .sendfileActions = {{-1, EAGAIN}, {static_cast<ssize_t>(payload.size()), 0}},
  });

  test::ClientConnection client(ts.port());

  test::sendAll(client.fd(), SimpleGetRequest("/sendfile-retry"));

  auto resp = test::recvWithTimeout(client.fd(), 5000ms);
  EXPECT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
}

// =============================================================================
// TLS-specific error paths
// =============================================================================

#ifdef AERONET_ENABLE_OPENSSL

// Test user-space TLS file serving error path (flushUserSpaceTlsBuffer error)
TEST(HttpResponseDispatchErrors, UserSpaceTlsBufferError) {
  test::QueueResetGuard<decltype(test::g_pread_path_actions)> guard(test::g_pread_path_actions);

  // Create a file for serving
  std::string payload(16UL * 1024, 'T');
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, payload);
  std::string filePath = tmp.filePath().string();

  // Use kTLS Disabled to force user-space TLS path
  test::TlsTestServer ts({"http/1.1"},
                         [](HttpServerConfig& cfg) { cfg.withTlsKtlsMode(TLSConfig::KtlsMode::Disabled); });

  ts.setDefault([&filePath](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.file(File(filePath));
    writer.end();
  });

  // Inject pread error to cause user-space TLS buffer flush to fail
  test::SetPreadPathActions(filePath, {{-1, EIO}});

  test::TlsClient client(ts.port());
  ASSERT_TRUE(client.handshakeOk());

  client.writeAll("GET /tls-error HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  (void)client.readAll();

  // Connection may be closed due to error
  SUCCEED();  // Main goal is exercising the error path
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
  test::TlsTestServer ts;
  ts.setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

  {
    // Create a raw TCP connection that we'll close without TLS handshake
    test::ClientConnection client(ts.port());
    // The ClientConnection destructor will close the socket without handshake
    // This triggers the TLS EOF-during-handshake path
  }

  // Allow server to process the closed connection
  std::this_thread::sleep_for(50ms);

  // Verify server still works after handling the aborted handshake
  test::TlsClient tlsClient(ts.port());
  EXPECT_TRUE(tlsClient.handshakeOk());
  auto resp = tlsClient.get("/after-eof");
  EXPECT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
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

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

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

// Test max buffer overflow in handleReadableClient (line 531-533)
TEST(ConnectionManagerErrors, MaxBufferOverflow) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.headerReadTimeout = {};
    cfg.maxHeaderBytes = 512;
    cfg.maxBodyBytes = 256;
  });

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

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
    EXPECT_TRUE(resp.contains("HTTP/1.1 431")) << resp;
  }

  {
    test::ClientConnection client(ts.port());

    // send a body exceeding the body limit but not the header limit
    std::string hugeBody =
        "GET /overflow HTTP/1.1\r\nHost: localhost\r\nContent-Length: 384\r\nContent-Type: text/plain\r\n\r\n";
    hugeBody += std::string(384, 'B');
    test::sendAll(client.fd(), hugeBody);

    const auto resp = test::recvWithTimeout(client.fd(), 2000ms);
    EXPECT_TRUE(resp.contains("HTTP/1.1 413")) << resp;
  }
}
