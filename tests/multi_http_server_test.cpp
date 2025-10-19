#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <string>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/multi-http-server.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;

namespace {

std::string SimpleGetRequest(std::string_view target, std::string_view connectionHeader = "close") {
  std::string req;
  req.reserve(128);
  req.append("GET ").append(target).append(" HTTP/1.1").append(aeronet::http::CRLF);
  req.append("Host: localhost").append(aeronet::http::CRLF);
  req.append("Connection: ").append(connectionHeader).append(aeronet::http::CRLF);
  req.append("Content-Length: 0").append(aeronet::http::DoubleCRLF);
  return req;
}

}  // namespace

TEST(MultiHttpServer, BeginDrainClosesKeepAliveConnections) {
  aeronet::HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  cfg.withReusePort();
  aeronet::MultiHttpServer multi(cfg, 2);
  const auto port = multi.port();

  multi.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest &req) {
    aeronet::HttpResponse resp;
    resp.body("OK");
    return resp;
  });

  multi.start();

  aeronet::test::ClientConnection cnx(port);
  const int fd = cnx.fd();

  ASSERT_TRUE(aeronet::test::sendAll(fd, SimpleGetRequest("/", "keep-alive")));
  const auto initial = aeronet::test::recvWithTimeout(fd);
  ASSERT_TRUE(initial.contains("Connection: keep-alive"));

  multi.beginDrain(200ms);
  EXPECT_TRUE(multi.isDraining());

  // Wait briefly for the listener to be closed by beginDrain() (avoid racey immediate connect attempts)
  EXPECT_TRUE(aeronet::test::WaitForListenerClosed(port, 210ms));

  ASSERT_TRUE(aeronet::test::sendAll(fd, SimpleGetRequest("/two", "keep-alive")));
  const auto drained = aeronet::test::recvWithTimeout(fd);
  EXPECT_TRUE(drained.contains("Connection: close"));

  EXPECT_TRUE(aeronet::test::WaitForPeerClose(fd, 500ms));

  multi.stop();
  EXPECT_FALSE(multi.isRunning());
}
