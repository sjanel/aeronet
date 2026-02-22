#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "aeronet/zerocopy-mode.hpp"

#define AERONET_WANT_SOCKET_OVERRIDES
#define AERONET_WANT_READ_WRITE_OVERRIDES

#include "aeronet/http-server-config.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

// Ignore SIGPIPE to prevent the test from being killed when writing to closed sockets
// (which can happen during the epoll failure simulation). SIGPIPE is raised when trying
// to write to a socket whose read end has been closed. Without this, test crashes are
// intermittent and hard to reproduce.
static const int kSigpipeIgnored = []() {
  ::signal(SIGPIPE, SIG_IGN);  // NOLINT(misc-include-cleaner)
  return 0;
}();

class HttpConnectDefaultConfig : public ::testing::Test {
 public:
  test::TestServer ts{HttpServerConfig{}};
  test::ClientConnection client{ts.port()};
  int fd{client.fd()};
};

// This test reproduces a partial-write scenario on the upstream side while tunneling.
// The upstream echo helper used elsewhere writes back immediately; here we start an
// upstream server that intentionally writes only a prefix on first recv to simulate
// a partial write (peer transportWrite returns smaller than input) and later drains
// the remainder. The server under test must append the remaining bytes to
// peer.tunnelOutBuffer and schedule the peer for writable events so data is
// eventually forwarded.
TEST_F(HttpConnectDefaultConfig, PartialWriteForwardsRemainingBytes) {
  (void)kSigpipeIgnored;  // Ensure SIGPIPE is ignored during this test
  // Use the test helper to start an echo server on loopback (returns ephemeral port).
  auto [sock, port] = test::startEchoServer();
  // Build CONNECT request to our upstream
  std::string req = "CONNECT 127.0.0.1:" + std::to_string(port) + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  ASSERT_GT(fd, 0);
  test::sendAll(fd, req, std::chrono::milliseconds{10000});
  auto resp = test::recvWithTimeout(fd, std::chrono::milliseconds{10000}, 93UL);
  EXPECT_TRUE(resp.contains("HTTP/1.1 200"));

  // Now send data through the tunnel and expect echo
  std::string_view simpleHello = "hello-tunnel";
  test::sendAll(fd, simpleHello, std::chrono::milliseconds{10000});
  auto echoedHello = test::recvWithTimeout(fd, std::chrono::milliseconds{10000}, simpleHello.size());
  EXPECT_EQ(echoedHello, simpleHello);

// Send payload that upstream will partially echo
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
  // We need a much smaller payload here otherwise the tests takes too long with additional memory checks
  std::string payload(1024UL * 1024, 'a');
#else
  std::string payload(16UL << 20, 'a');
#endif
  test::sendAll(fd, payload, std::chrono::milliseconds{10000});

  // Wait to receive the full payload (some arrives quickly, remainder after upstream sleeps)
  auto echoed = test::recvWithTimeout(fd, std::chrono::milliseconds{10000}, payload.size());
  EXPECT_TRUE(echoed.starts_with("aaaaaaaaaaaaaaaaaa"));
  EXPECT_TRUE(echoed.ends_with("aaaaaaaaaaaaaaaaaa"));
  EXPECT_EQ(echoed.size(), payload.size());
  EXPECT_TRUE(echoed.contains(payload));

  // now simulate some epoll mod failures, server should be able to recover from these
  test::EventLoopHookGuard guard;
  test::FailAllEpollCtlMod(EACCES);
  test::sendAll(fd, payload, std::chrono::milliseconds{5000});

  // Get out of the recv as soon as we receive some data to decrease the unit test time, but don't assert anything here
  test::recvWithTimeout(fd, std::chrono::milliseconds{500}, 16UL);
}

TEST_F(HttpConnectDefaultConfig, DnsFailureReturns502) {
  std::string req = "CONNECT no-such-host.example.invalid:80 HTTP/1.1\r\nHost: no-such-host.example.invalid\r\n\r\n";
  ASSERT_GT(fd, 0);
  test::sendAll(fd, req);
  auto resp = test::recvWithTimeout(fd, std::chrono::milliseconds{500});
  // Expect 502 Bad Gateway or connection close
  ASSERT_TRUE(resp.contains("502") || resp.empty());
}

TEST_F(HttpConnectDefaultConfig, AllowlistRejectsTarget) {
  // only allow example.com
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    std::vector<std::string> list = {"example.com"};
    cfg.withConnectAllowlist(list.begin(), list.end());
  });

  std::string req = "CONNECT 127.0.0.1:80 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";

  ASSERT_GT(fd, 0);
  test::sendAll(fd, req);
  auto resp = test::recvWithTimeout(fd, std::chrono::milliseconds{500});
  ASSERT_TRUE(resp.contains("403") || resp.contains("CONNECT target not allowed"));
}

TEST_F(HttpConnectDefaultConfig, MalformedConnectTargetReturns400) {
  // Missing ':' in authority form -> should return 400 Bad Request
  std::string req = "CONNECT malformed-target HTTP/1.1\r\nHost: malformed-target\r\n\r\n";
  ASSERT_GT(fd, 0);
  test::sendAll(fd, req);
  auto resp = test::recvWithTimeout(fd, std::chrono::milliseconds{500});
  ASSERT_TRUE(resp.contains("HTTP/1.1 400") || resp.contains("Malformed CONNECT target"));
}

// Test that closing a tunnel connection also cleans up the peer connection.
// This exercises the closeConnection() path at lines 414-429 in connection-manager.cpp
// where peerFd != -1 triggers peer lookup and cleanup.
TEST(HttpConnectTunnelCleanup, TunnelPeerCleanupOnClientClose) {
  test::TestServer ts{HttpServerConfig{}};

  // Start an echo server to act as upstream
  auto [sock, port] = test::startEchoServer();

  {
    test::ClientConnection client(ts.port());
    int fd = client.fd();

    // Establish the CONNECT tunnel
    std::string req = "CONNECT 127.0.0.1:" + std::to_string(port) + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    ASSERT_GT(fd, 0);
    test::sendAll(fd, req, 5000ms);
    auto resp = test::recvWithTimeout(fd, 5000ms, 93UL);
    EXPECT_TRUE(resp.contains("HTTP/1.1 200"));

    // Verify tunnel works by sending and receiving data
    std::string_view testData = "tunnel-peer-test";
    test::sendAll(fd, testData, 2000ms);
    auto echoed = test::recvWithTimeout(fd, 2000ms, testData.size());
    EXPECT_TRUE(echoed.contains(testData));

    // Client goes out of scope here, closing the fd and triggering
    // closeConnection() with peerFd != -1. The server detects the
    // client close and cleans up both connection states.
  }

  // Give server time to process the close and cleanup
  std::this_thread::sleep_for(50ms);

  // Server should still be operational after tunnel cleanup
  test::ClientConnection client2(ts.port());
  std::string req2 = "GET / HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
  test::sendAll(client2.fd(), req2, 1000ms);
  auto resp2 = test::recvWithTimeout(client2.fd(), 1000ms);
  // 404 is fine - we just need to verify server is still responsive
  EXPECT_TRUE(resp2.contains("HTTP/1.1")) << resp2;
}
