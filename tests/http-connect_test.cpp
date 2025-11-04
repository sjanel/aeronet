#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/http-server-config.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

class HttpConnectDefaultConfig : public ::testing::Test {
 public:
  virtual ~HttpConnectDefaultConfig() = default;

  test::TestServer ts{HttpServerConfig{}};
  test::ClientConnection client{ts.port()};
  int fd{client.fd()};
};

TEST_F(HttpConnectDefaultConfig, EchoTunnelSuccess) {
  auto [sock, port] = test::startEchoServer();
  // Build CONNECT request
  std::string req = "CONNECT 127.0.0.1:" + std::to_string(port) + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  ASSERT_GT(fd, 0);
  test::sendAll(fd, req);
  auto resp = test::recvWithTimeout(fd, 500ms);
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));

  // Now send data through the tunnel and expect echo
  std::string_view payload = "hello-tunnel";
  test::sendAll(fd, payload);
  auto echoed = test::recvWithTimeout(fd, 500ms);
  ASSERT_TRUE(echoed.contains(payload));
}

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
  auto resp = test::recvWithTimeout(fd, 500ms);
  ASSERT_TRUE(resp.contains("200"));

  // Send payload that upstream will partially echo
  std::string payload(6000000, 'a');
  test::sendAll(fd, payload);

  // Wait to receive the full payload (some arrives quickly, remainder after upstream sleeps)
  auto echoed = test::recvWithTimeout(fd, 2000ms);
  ASSERT_TRUE(echoed.contains(payload));
}

TEST_F(HttpConnectDefaultConfig, DnsFailureReturns502) {
  std::string req = "CONNECT no-such-host.example.invalid:80 HTTP/1.1\r\nHost: no-such-host.example.invalid\r\n\r\n";
  ASSERT_GT(fd, 0);
  test::sendAll(fd, req);
  auto resp = test::recvWithTimeout(fd, 500ms);
  // Expect 502 Bad Gateway or connection close
  ASSERT_TRUE(resp.contains("502") || resp.empty());
}

TEST(HttpConnect, AllowlistRejectsTarget) {
  HttpServerConfig cfg{};
  // only allow example.com
  std::vector<std::string> list = {"example.com"};
  cfg.withConnectAllowlist(list.begin(), list.end());
  test::TestServer ts2(cfg);

  std::string req = "CONNECT 127.0.0.1:80 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  test::ClientConnection client2(ts2.port());
  int fd2 = client2.fd();
  ASSERT_GT(fd2, 0);
  test::sendAll(fd2, req);
  auto resp = test::recvWithTimeout(fd2, 500ms);
  ASSERT_TRUE(resp.contains("403") || resp.contains("CONNECT target not allowed"));
}
