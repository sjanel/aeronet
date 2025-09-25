#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "http-method-set.hpp"
#include "http-method.hpp"
#include "test_http_client.hpp"

using namespace aeronet;

TEST(HttpRouting, BasicPathDispatch) {
  HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(10);
  HttpServer server(cfg);
  http::MethodSet helloMethods{http::Method::GET};
  server.addPathHandler("/hello", helloMethods, [](const HttpRequest&) {
    HttpResponse resp;
    resp.statusCode = 200;
    resp.reason = "OK";
    resp.body = "world";
    resp.contentType = "text/plain";
    return resp;
  });
  http::MethodSet multiMethods{http::Method::GET, http::Method::POST};
  server.addPathHandler("/multi", multiMethods, [](const HttpRequest& req) {
    HttpResponse resp;
    resp.statusCode = 200;
    resp.reason = "OK";
    resp.body = std::string(req.method) + "!";
    resp.contentType = "text/plain";
    return resp;
  });

  std::atomic<bool> done{false};
  std::jthread th([&]() { server.runUntil([&]() { return done.load(); }); });
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  test_http_client::RequestOptions getHello;
  getHello.method = "GET";
  getHello.target = "/hello";
  auto resp1 = test_http_client::request_or_throw(server.port(), getHello);
  EXPECT_NE(resp1.find("200 OK"), std::string::npos);
  EXPECT_NE(resp1.find("world"), std::string::npos);
  test_http_client::RequestOptions postHello;
  postHello.method = "POST";
  postHello.target = "/hello";
  postHello.headers.emplace_back("Content-Length", "0");
  auto resp2 = test_http_client::request_or_throw(server.port(), postHello);
  EXPECT_NE(resp2.find("405 Method Not Allowed"), std::string::npos);
  test_http_client::RequestOptions getMissing;
  getMissing.method = "GET";
  getMissing.target = "/missing";
  auto resp3 = test_http_client::request_or_throw(server.port(), getMissing);
  EXPECT_NE(resp3.find("404 Not Found"), std::string::npos);
  test_http_client::RequestOptions postMulti;
  postMulti.method = "POST";
  postMulti.target = "/multi";
  postMulti.headers.emplace_back("Content-Length", "0");
  auto resp4 = test_http_client::request_or_throw(server.port(), postMulti);
  EXPECT_NE(resp4.find("200 OK"), std::string::npos);
  EXPECT_NE(resp4.find("POST!"), std::string::npos);

  done.store(true);
}

TEST(HttpRouting, GlobalFallbackWithPathHandlers) {
  HttpServerConfig cfg;
  HttpServer server(cfg);
  server.setHandler([](const HttpRequest&) {
    HttpResponse resp;
    resp.statusCode = 200;
    resp.reason = "OK";
    return resp;
  });
  // Adding path handler after global handler is now allowed (Phase 2 mixing model)
  http::MethodSet xMethods{http::Method::GET};
  EXPECT_NO_THROW(server.addPathHandler("/x", xMethods, [](const HttpRequest&) { return HttpResponse{}; }));
}
