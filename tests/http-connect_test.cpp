#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#define AERONET_WANT_SOCKET_OVERRIDES
#define AERONET_WANT_READ_WRITE_OVERRIDES

#include "aeronet/http-server-config.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

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
  // Use the test helper to start an echo server on loopback (returns ephemeral port).
  auto [sock, port] = test::startEchoServer();
  // Build CONNECT request to our upstream
  std::string req = "CONNECT 127.0.0.1:" + std::to_string(port) + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  ASSERT_GT(fd, 0);
  test::sendAll(fd, req);
  auto resp = test::recvWithTimeout(fd, std::chrono::milliseconds{500});
  EXPECT_TRUE(resp.contains("HTTP/1.1 200"));

  // Now send data through the tunnel and expect echo
  std::string_view simpleHello = "hello-tunnel";
  test::sendAll(fd, simpleHello);
  auto echoedHello = test::recvWithTimeout(fd, std::chrono::milliseconds{500});
  EXPECT_TRUE(echoedHello.contains(simpleHello));

  // Send payload that upstream will partially echo
  std::string payload(16UL * 1024 * 1024, 'a');
  test::sendAll(fd, payload);

  // Wait to receive the full payload (some arrives quickly, remainder after upstream sleeps)
  auto echoed = test::recvWithTimeout(fd, std::chrono::milliseconds{1500});
  EXPECT_TRUE(echoed.contains(payload));

  // now simulate some epoll mod failures, server should be able to recover from these
  test::FailAllEpollCtlMod(EACCES);
  test::sendAll(fd, payload);

  // Wait to receive the full payload (some arrives quickly, remainder after upstream sleeps)
  test::recvWithTimeout(fd, std::chrono::milliseconds{1500});

  test::ResetEpollCtlModFail();
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
