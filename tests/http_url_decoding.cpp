#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "http-constants.hpp"
#include "http-method-set.hpp"
#include "http-method.hpp"
#include "test_http_client.hpp"

using namespace aeronet;

// (Removed old raw helper; switched to shared test_http_client::request for all requests)

TEST(HttpUrlDecoding, SpaceDecoding) {
  HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(4);
  HttpServer server(cfg);
  http::MethodSet ms{http::Method::GET};
  server.addPathHandler("/hello world", ms, [](const HttpRequest &req) {
    return aeronet::HttpResponse(200)
        .reason("OK")
        .body(std::string(req.target))
        .contentType(http::ContentTypeTextPlain);
  });
  std::atomic<bool> done{false};
  std::jthread th([&] { server.runUntil([&] { return done.load(); }); });
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  test_http_client::RequestOptions optHello;
  optHello.method = "GET";
  optHello.target = "/hello%20world";
  auto respOwned = test_http_client::request_or_throw(server.port(), optHello);
  std::string_view resp = respOwned;
  EXPECT_NE(resp.find("200 OK"), std::string_view::npos);
  EXPECT_NE(resp.find("hello world"), std::string_view::npos);
  done.store(true);
}

TEST(HttpUrlDecoding, Utf8Decoded) {
  HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(4);
  HttpServer server(cfg);
  http::MethodSet ms{http::Method::GET};
  // Path contains snowman + space + 'x'
  std::string decodedPath = "/\xE2\x98\x83 x";  // /☃ x
  server.addPathHandler(decodedPath, ms, [](const HttpRequest &) {
    return aeronet::HttpResponse(200).reason("OK").body("utf8").contentType(http::ContentTypeTextPlain);
  });
  std::atomic<bool> done{false};
  std::jthread th([&] { server.runUntil([&] { return done.load(); }); });
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  // Percent-encoded UTF-8 for snowman (E2 98 83) plus %20 and 'x'
  test_http_client::RequestOptions optUtf8;
  optUtf8.method = "GET";
  optUtf8.target = "/%E2%98%83%20x";
  auto respOwned = test_http_client::request_or_throw(server.port(), optUtf8);
  std::string_view resp = respOwned;
  EXPECT_NE(resp.find("200 OK"), std::string_view::npos);
  EXPECT_NE(resp.find("utf8"), std::string_view::npos);
  done.store(true);
}

TEST(HttpUrlDecoding, PlusIsNotSpace) {
  HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(4);
  HttpServer server(cfg);
  http::MethodSet ms{http::Method::GET};
  server.addPathHandler("/a+b", ms, [](const HttpRequest &) {
    return aeronet::HttpResponse(200).reason("OK").body("plus").contentType(http::ContentTypeTextPlain);
  });
  std::atomic<bool> done{false};
  std::jthread th([&] { server.runUntil([&] { return done.load(); }); });
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  test_http_client::RequestOptions optPlus;
  optPlus.method = "GET";
  optPlus.target = "/a+b";
  auto respOwned = test_http_client::request_or_throw(server.port(), optPlus);
  std::string_view resp = respOwned;
  EXPECT_NE(resp.find("200 OK"), std::string_view::npos);
  EXPECT_NE(resp.find("plus"), std::string_view::npos);
  done.store(true);
}

TEST(HttpUrlDecoding, InvalidPercentSequence400) {
  HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(2);
  HttpServer server(cfg);
  std::atomic<bool> done{false};
  std::jthread th([&] { server.runUntil([&] { return done.load(); }); });
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  test_http_client::RequestOptions optBad;
  optBad.method = "GET";
  optBad.target = "/bad%G1";
  auto respOwned = test_http_client::request_or_throw(server.port(), optBad);
  std::string_view resp = respOwned;
  EXPECT_NE(resp.find("400 Bad Request"), std::string_view::npos);
  done.store(true);
}
