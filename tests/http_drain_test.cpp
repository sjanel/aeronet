#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;

namespace {

std::string SimpleGetRequest(std::string_view target, std::string_view connectionHeader = "close") {
  std::string req;
  req.reserve(128);
  req.append("GET ").append(target).append(" HTTP/1.1\r\n");
  req.append("Host: localhost\r\n");
  req.append("Connection: ").append(connectionHeader).append("\r\n");
  req.append("Content-Length: 0\r\n\r\n");
  return req;
}

bool WaitForServerRunning(aeronet::HttpServer& server, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
    if (server.isRunning()) {
      return true;
    }
  }
  return server.isRunning();
}

}  // namespace

TEST(HttpDrain, StopsNewConnections) {
  aeronet::HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  aeronet::test::TestServer ts(cfg);

  ts.server.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("OK");
    return resp;
  });

  const auto port = ts.port();

  ASSERT_TRUE(aeronet::test::AttemptConnect(port));

  // Baseline request to ensure server responds prior to draining.
  {
    aeronet::test::ClientConnection cnx(port);
    ASSERT_TRUE(aeronet::test::sendAll(cnx.fd(), SimpleGetRequest("/pre", "keep-alive")));
    const auto resp = aeronet::test::recvWithTimeout(cnx.fd());
    EXPECT_TRUE(resp.contains("200"));
  }

  ts.server.beginDrain();

  EXPECT_FALSE(aeronet::test::AttemptConnect(port));

  ts.stop();
}

TEST(HttpDrain, KeepAliveConnectionsCloseAfterDrain) {
  aeronet::HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  aeronet::test::TestServer ts(cfg);

  ts.server.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("OK");
    return resp;
  });

  const auto port = ts.port();
  aeronet::test::ClientConnection cnx(port);
  const int fd = cnx.fd();

  ASSERT_TRUE(aeronet::test::sendAll(fd, SimpleGetRequest("/one", "keep-alive")));
  auto firstResponse = aeronet::test::recvWithTimeout(fd);
  ASSERT_TRUE(firstResponse.contains("Connection: keep-alive"));

  ts.server.beginDrain();

  ASSERT_TRUE(aeronet::test::sendAll(fd, SimpleGetRequest("/two", "keep-alive")));
  auto drainedResponse = aeronet::test::recvWithTimeout(fd);
  EXPECT_TRUE(drainedResponse.contains("Connection: close"));

  EXPECT_TRUE(aeronet::test::WaitForPeerClose(fd, 500ms));

  ts.stop();
}

TEST(HttpDrain, DeadlineForcesIdleConnectionsToClose) {
  aeronet::HttpServerConfig cfg;
  cfg.keepAliveTimeout = 5s;  // ensure default timeout does not interfere with the test window
  aeronet::test::TestServer ts(cfg);

  ts.server.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("OK");
    return resp;
  });

  const auto port = ts.port();
  aeronet::test::ClientConnection idle(port);
  const int fd = idle.fd();

  ASSERT_TRUE(WaitForServerRunning(ts.server, 200ms));
  ts.server.beginDrain(std::chrono::milliseconds{50});
  ASSERT_TRUE(ts.server.isDraining());

  EXPECT_TRUE(aeronet::test::WaitForPeerClose(fd, 500ms));

  ts.stop();
}
