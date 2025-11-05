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
using namespace aeronet;

TEST(HttpKeepAlive, MultipleSequentialRequests) {
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest& req) {
    HttpResponse resp;
    resp.body(std::string("ECHO") + std::string(req.path()));
    return resp;
  });

  test::ClientConnection cnx(port);
  int fd = cnx.fd();

  std::string req1 = "GET /one HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n";
  test::sendAll(fd, req1);
  std::string resp1 = test::recvWithTimeout(fd);
  EXPECT_TRUE(resp1.contains("ECHO/one"));
  EXPECT_TRUE(resp1.contains("Connection: keep-alive"));

  std::string req2 = "GET /two HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";  // implicit keep-alive
  test::sendAll(fd, req2);
  std::string resp2 = test::recvWithTimeout(fd);
  EXPECT_TRUE(resp2.contains("ECHO/two"));
}
