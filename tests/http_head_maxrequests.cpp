#include <gtest/gtest.h>

#include <string>
#include <thread>

#include "aeronet/server.hpp"
#include "test_util.hpp"
using namespace std::chrono_literals;

TEST(HttpHead, MaxRequestsApplied) {
  uint16_t port = 18610;
  aeronet::ServerConfig cfg;
  cfg.withPort(port).withMaxRequestsPerConnection(3);
  aeronet::HttpServer server(cfg);
  server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body = "IGNORED";
    return resp;
  });
  std::thread th([&] { server.runUntil([] { return false; }, 30ms); });
  std::this_thread::sleep_for(60ms);
  int fd = tu_connect(port);
  ASSERT_GE(fd, 0);
  // 4 HEAD requests pipelined; only 3 responses expected then close
  std::string reqs;
  for (int i = 0; i < 4; ++i) {
    reqs += "HEAD /h" + std::to_string(i) + " HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
  }
  tu_sendAll(fd, reqs);
  std::string resp = tu_recvUntilClosed(fd);
  server.stop();
  th.join();
  int statusCount = 0;
  size_t pos = 0;
  while ((pos = resp.find("HTTP/1.1 200", pos)) != std::string::npos) {
    ++statusCount;
    pos += 11;
  }
  ASSERT_EQ(3, statusCount) << resp;
  // HEAD responses must not include body; ensure no accidental body token present
  ASSERT_EQ(std::string::npos, resp.find("IGNORED"));
}
