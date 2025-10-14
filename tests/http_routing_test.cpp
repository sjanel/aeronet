#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

TEST(HttpRouting, BasicPathDispatch) {
  HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(10);
  HttpServer server(cfg);
  server.router().setPath("/hello", http::Method::GET, [](const HttpRequest&) {
    return HttpResponse(200, "OK").body("world").contentType(aeronet::http::ContentTypeTextPlain);
  });
  server.router().setPath("/multi", http::Method::GET | http::Method::POST, [](const HttpRequest& req) {
    return HttpResponse(200, "OK")
        .body(std::string(http::toMethodStr(req.method())) + "!")
        .contentType(aeronet::http::ContentTypeTextPlain);
  });

  std::atomic<bool> done{false};
  std::jthread th([&]() { server.runUntil([&]() { return done.load(); }); });
  for (int i = 0; i < 200 && !server.isRunning(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  aeronet::test::RequestOptions getHello;
  getHello.method = "GET";
  getHello.target = "/hello";
  auto resp1 = aeronet::test::requestOrThrow(server.port(), getHello);
  EXPECT_TRUE(resp1.contains("200 OK"));
  EXPECT_TRUE(resp1.contains("world"));
  aeronet::test::RequestOptions postHello;
  postHello.method = "POST";
  postHello.target = "/hello";
  postHello.headers.emplace_back("Content-Length", "0");
  auto resp2 = aeronet::test::requestOrThrow(server.port(), postHello);
  EXPECT_TRUE(resp2.contains("405 Method Not Allowed"));
  aeronet::test::RequestOptions getMissing;
  getMissing.method = "GET";
  getMissing.target = "/missing";
  auto resp3 = aeronet::test::requestOrThrow(server.port(), getMissing);
  EXPECT_TRUE(resp3.contains("404 Not Found"));
  aeronet::test::RequestOptions postMulti;
  postMulti.method = "POST";
  postMulti.target = "/multi";
  postMulti.headers.emplace_back("Content-Length", "0");
  auto resp4 = aeronet::test::requestOrThrow(server.port(), postMulti);
  EXPECT_TRUE(resp4.contains("200 OK"));
  EXPECT_TRUE(resp4.contains("POST!"));

  done.store(true);
}

TEST(HttpRouting, GlobalFallbackWithPathHandlers) {
  HttpServerConfig cfg;
  HttpServer server(cfg);
  server.router().setDefault([](const HttpRequest&) { return HttpResponse(200, "OK"); });
  // Adding path handler after global handler is now allowed (Phase 2 mixing model)
  EXPECT_NO_THROW(
      server.router().setPath("/x", http::Method::GET, [](const HttpRequest&) { return HttpResponse(200); }));
}
