#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_util.hpp"

TEST(HttpHead, MaxRequestsApplied) {
  aeronet::HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(3);
  aeronet::HttpServer server(cfg);
  auto port = server.port();
  server.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body("IGNORED");
    return resp;
  });
  std::jthread th([&] { server.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  aeronet::test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  // 4 HEAD requests pipelined; only 3 responses expected then close
  std::string reqs;
  for (int i = 0; i < 4; ++i) {
    reqs += "HEAD /h" + std::to_string(i) + " HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
  }
  aeronet::test::sendAll(fd, reqs);
  std::string resp = aeronet::test::recvUntilClosed(fd);
  server.stop();
  int statusCount = 0;
  std::size_t pos = 0;
  while ((pos = resp.find("HTTP/1.1 200", pos)) != std::string::npos) {
    ++statusCount;
    pos += 11;
  }
  ASSERT_EQ(3, statusCount) << resp;
  // HEAD responses must not include body; ensure no accidental body token present
  ASSERT_EQ(std::string::npos, resp.find("IGNORED"));
}
