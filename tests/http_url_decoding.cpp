// Tests for server-side percent-decoding of request target in parser.
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
std::string doRaw(uint16_t port, const std::string &raw) {
  int fd = tu_connect(port);
  if (fd < 0) {
    return {};
  }
  if (!tu_sendAll(fd, raw)) {
    ::close(fd);
    return {};
  }
  auto resp = tu_recvUntilClosed(fd);
  ::close(fd);
  return resp;
}
}  // namespace

TEST(HttpUrlDecoding, SpaceDecoding) {
  ServerConfig cfg;
  cfg.withMaxRequestsPerConnection(4);
  HttpServer server(cfg);
  http::MethodSet ms{http::Method::GET};
  server.addPathHandler("/hello world", ms, [](const HttpRequest &req) {
    HttpResponse resp;
    resp.statusCode = 200;
    resp.reason = "OK";
    resp.body = std::string(req.target);
    resp.contentType = "text/plain";
    return resp;
  });
  std::atomic<bool> done{false};
  std::jthread th([&] { server.runUntil([&] { return done.load(); }, std::chrono::milliseconds{50}); });
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  auto resp = doRaw(server.port(), "GET /hello%20world HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("hello world"), std::string::npos);
  done.store(true);
  th.join();
}

TEST(HttpUrlDecoding, Utf8Decoded) {
  ServerConfig cfg;
  cfg.withMaxRequestsPerConnection(4);
  HttpServer server(cfg);
  http::MethodSet ms{http::Method::GET};
  // Path contains snowman + space + 'x'
  std::string decodedPath = "/\xE2\x98\x83 x";  // /☃ x
  server.addPathHandler(decodedPath, ms, [](const HttpRequest &) {
    HttpResponse resp;
    resp.statusCode = 200;
    resp.reason = "OK";
    resp.body = "utf8";
    resp.contentType = "text/plain";
    return resp;
  });
  std::atomic<bool> done{false};
  std::jthread th([&] { server.runUntil([&] { return done.load(); }, std::chrono::milliseconds{50}); });
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  // Percent-encoded UTF-8 for snowman (E2 98 83) plus %20 and 'x'
  auto resp = doRaw(server.port(), "GET /%E2%98%83%20x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("utf8"), std::string::npos);
  done.store(true);
  th.join();
}

TEST(HttpUrlDecoding, PlusIsNotSpace) {
  ServerConfig cfg;
  cfg.withMaxRequestsPerConnection(4);
  HttpServer server(cfg);
  http::MethodSet ms{http::Method::GET};
  server.addPathHandler("/a+b", ms, [](const HttpRequest &) {
    HttpResponse resp;
    resp.statusCode = 200;
    resp.reason = "OK";
    resp.body = "plus";
    resp.contentType = "text/plain";
    return resp;
  });
  std::atomic<bool> done{false};
  std::jthread th([&] { server.runUntil([&] { return done.load(); }, std::chrono::milliseconds{50}); });
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  auto resp = doRaw(server.port(), "GET /a+b HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("plus"), std::string::npos);
  done.store(true);
  th.join();
}

TEST(HttpUrlDecoding, InvalidPercentSequence400) {
  ServerConfig cfg;
  cfg.withMaxRequestsPerConnection(2);
  HttpServer server(cfg);
  std::atomic<bool> done{false};
  std::jthread th([&] { server.runUntil([&] { return done.load(); }, std::chrono::milliseconds{50}); });
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  auto resp = doRaw(server.port(), "GET /bad%G1 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  EXPECT_NE(resp.find("400 Bad Request"), std::string::npos);
  done.store(true);
  th.join();
}
