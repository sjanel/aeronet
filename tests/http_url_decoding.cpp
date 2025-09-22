// Tests for server-side percent-decoding of request target in parser.
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "http-method-set.hpp"
#include "http-method.hpp"
#include "test_http_client.hpp"

using namespace aeronet;

// (Removed old raw helper; switched to shared test_http_client::request for all requests)

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
  test_http_client::RequestOptions optHello;
  optHello.method = "GET";
  optHello.target = "/hello%20world";
  auto respOpt = test_http_client::request(server.port(), optHello);
  ASSERT_TRUE(respOpt.has_value());
  const std::string &resp = *respOpt;
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
  test_http_client::RequestOptions optUtf8;
  optUtf8.method = "GET";
  optUtf8.target = "/%E2%98%83%20x";
  auto respOpt = test_http_client::request(server.port(), optUtf8);
  ASSERT_TRUE(respOpt.has_value());
  const std::string &resp = *respOpt;
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
  test_http_client::RequestOptions optPlus;
  optPlus.method = "GET";
  optPlus.target = "/a+b";
  auto respOpt = test_http_client::request(server.port(), optPlus);
  ASSERT_TRUE(respOpt.has_value());
  const std::string &resp = *respOpt;
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
  test_http_client::RequestOptions optBad;
  optBad.method = "GET";
  optBad.target = "/bad%G1";
  auto respOpt = test_http_client::request(server.port(), optBad);
  ASSERT_TRUE(respOpt.has_value());
  const std::string &resp = *respOpt;
  EXPECT_NE(resp.find("400 Bad Request"), std::string::npos);
  done.store(true);
  th.join();
}
