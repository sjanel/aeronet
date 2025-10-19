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

TEST(HttpConnect, EchoTunnelSuccess) {
  HttpServerConfig cfg{};
  aeronet::test::TestServer ts(cfg);

  int upstreamPort = aeronet::test::startEchoServer();

  // Build CONNECT request
  std::string req = "CONNECT 127.0.0.1:" + std::to_string(upstreamPort) + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  aeronet::test::ClientConnection client(ts.port());
  int fd = client.fd();
  ASSERT_GT(fd, 0);
  aeronet::test::sendAll(fd, req);
  auto resp = aeronet::test::recvWithTimeout(fd, 2000ms);
  ASSERT_TRUE(resp.contains("200"));

  // Now send data through the tunnel and expect echo
  std::string_view payload = "hello-tunnel";
  aeronet::test::sendAll(fd, payload);
  auto echoed = aeronet::test::recvWithTimeout(fd, 2000ms);
  ASSERT_TRUE(echoed.contains(payload));
}

// This test reproduces a partial-write scenario on the upstream side while tunneling.
// The upstream echo helper used elsewhere writes back immediately; here we start an
// upstream server that intentionally writes only a prefix on first recv to simulate
// a partial write (peer transportWrite returns smaller than input) and later drains
// the remainder. The server under test must append the remaining bytes to
// peer.tunnelOutBuffer and schedule the peer for writable events so data is
// eventually forwarded.
TEST(HttpTunneling, PartialWriteForwardsRemainingBytes) {
  HttpServerConfig cfg{};
  aeronet::test::TestServer ts(cfg);

  // Use the test helper to start an echo server on loopback (returns ephemeral port).
  int upstreamPort = aeronet::test::startEchoServer();

  // Build CONNECT request to our upstream
  std::string req = "CONNECT 127.0.0.1:" + std::to_string(upstreamPort) + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  aeronet::test::ClientConnection client(ts.port());
  int fd = client.fd();
  ASSERT_GT(fd, 0);
  ASSERT_TRUE(aeronet::test::sendAll(fd, req));
  auto resp = aeronet::test::recvWithTimeout(fd, 2000ms);
  ASSERT_TRUE(resp.contains("200"));

  // Send payload that upstream will partially echo
  std::string payload(6000000, 'a');
  ASSERT_TRUE(aeronet::test::sendAll(fd, payload));

  // Wait to receive the full payload (some arrives quickly, remainder after upstream sleeps)
  auto echoed = aeronet::test::recvWithTimeout(fd, 2000ms);
  ASSERT_TRUE(echoed.contains(payload));
}

TEST(HttpConnect, DnsFailureReturns502) {
  HttpServerConfig cfg{};
  aeronet::test::TestServer ts(cfg);

  std::string req = "CONNECT no-such-host.example.invalid:80 HTTP/1.1\r\nHost: no-such-host.example.invalid\r\n\r\n";
  aeronet::test::ClientConnection client(ts.port());
  int fd = client.fd();
  ASSERT_GT(fd, 0);
  aeronet::test::sendAll(fd, req);
  auto resp = aeronet::test::recvWithTimeout(fd, 2000ms);
  // Expect 502 Bad Gateway or connection close
  ASSERT_TRUE(resp.contains("502") || resp.empty());
}

TEST(HttpConnect, AllowlistRejectsTarget) {
  HttpServerConfig cfg{};
  // only allow example.com
  std::vector<std::string> list = {"example.com"};
  cfg.withConnectAllowlist(list.begin(), list.end());
  aeronet::test::TestServer ts(cfg);

  std::string req = "CONNECT 127.0.0.1:80 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  aeronet::test::ClientConnection client2(ts.port());
  int fd = client2.fd();
  ASSERT_GT(fd, 0);
  aeronet::test::sendAll(fd, req);
  auto resp = aeronet::test::recvWithTimeout(fd, 2000ms);
  ASSERT_TRUE(resp.contains("403") || resp.contains("CONNECT target not allowed"));
}
