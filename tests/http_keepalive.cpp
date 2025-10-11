#include <gtest/gtest.h>

#include <cerrno>
#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;

TEST(HttpKeepAlive, MultipleSequentialRequests) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body(std::string("ECHO") + std::string(req.path()));
    return resp;
  });

  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();

  std::string req1 = "GET /one HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n";
  aeronet::test::sendAll(fd, req1);
  std::string resp1 = aeronet::test::recvWithTimeout(fd);
  EXPECT_NE(std::string::npos, resp1.find("ECHO/one"));
  EXPECT_NE(std::string::npos, resp1.find("Connection: keep-alive"));

  std::string req2 = "GET /two HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";  // implicit keep-alive
  aeronet::test::sendAll(fd, req2);
  std::string resp2 = aeronet::test::recvWithTimeout(fd);
  EXPECT_NE(std::string::npos, resp2.find("ECHO/two"));
}

TEST(HttpLimits, RejectHugeHeaders) {
  aeronet::HttpServerConfig cfg;
  cfg.maxHeaderBytes = 128;
  cfg.enableKeepAlive = false;
  aeronet::test::TestServer ts(cfg);
  auto port = ts.port();
  ts.server.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("OK");
    return resp;
  });

  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();

  std::string bigHeader(200, 'a');
  std::string req = "GET /h HTTP/1.1\r\nHost: x\r\nX-Big: " + bigHeader + "\r\n\r\n";
  aeronet::test::sendAll(fd, req);
  std::string resp = aeronet::test::recvWithTimeout(fd);
  EXPECT_NE(std::string::npos, resp.find("431"));
}
