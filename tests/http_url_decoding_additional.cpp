#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "http-method-set.hpp"
#include "http-method.hpp"
#include "test_util.hpp"

using namespace aeronet;

namespace {
std::string raw(uint16_t port, const std::string& req) {
  int fd = tu_connect(port);
  if (fd < 0) {
    return {};
  }
  if (!tu_sendAll(fd, req)) {
    ::close(fd);
    return {};
  }
  auto resp = tu_recvUntilClosed(fd);
  ::close(fd);
  return resp;
}
}  // namespace

TEST(HttpUrlDecodingExtra, IncompletePercentSequence400) {
  ServerConfig cfg;
  cfg.withMaxRequestsPerConnection(1);
  HttpServer server(cfg);
  std::atomic<bool> done = false;
  std::jthread th([&] { server.runUntil([&] { return done.load(); }, std::chrono::milliseconds{50}); });
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  auto resp = raw(server.port(), "GET /bad% HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  EXPECT_NE(resp.find("400 Bad Request"), std::string::npos);
  done = true;  // jthread auto-joins on destruction
}

TEST(HttpUrlDecodingExtra, MixedSegmentsDecoding) {
  ServerConfig cfg;
  cfg.withMaxRequestsPerConnection(2);
  HttpServer server(cfg);
  http::MethodSet ms{http::Method::GET};
  server.addPathHandler("/seg one/part%/two", ms, [](const HttpRequest& req) {
    HttpResponse resp;
    resp.statusCode = 200;
    resp.reason = "OK";
    resp.body = std::string(req.target);
    resp.contentType = "text/plain";
    return resp;
  });
  std::atomic<bool> done = false;
  std::jthread th([&] { server.runUntil([&] { return done.load(); }, std::chrono::milliseconds{50}); });
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  // encodes space in first segment only
  auto resp = raw(server.port(), "GET /seg%20one/part%25/two HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("/seg one/part%/two"), std::string::npos);
  done = true;  // jthread auto-joins on destruction
}
