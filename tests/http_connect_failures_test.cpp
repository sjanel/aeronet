#include <gtest/gtest.h>

#include <chrono>

#include "aeronet/http-server-config.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;
using namespace std::chrono_literals;

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
