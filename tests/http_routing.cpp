#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "http-method-set.hpp"
#include "http-method.hpp"
#include "test_http_client.hpp"

using namespace aeronet;

TEST(HttpRouting, BasicPathDispatch) {
  ServerConfig cfg;
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
  std::jthread th([&]() { server.runUntil([&]() { return done.load(); }, std::chrono::milliseconds{50}); });
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  test_http_client::RequestOptions getHello;
  getHello.method = "GET";
  getHello.target = "/hello";
  auto resp1Opt = test_http_client::request(server.port(), getHello);
  ASSERT_TRUE(resp1Opt.has_value());
  const std::string& resp1 = *resp1Opt;
  EXPECT_NE(resp1.find("200 OK"), std::string::npos);
  EXPECT_NE(resp1.find("world"), std::string::npos);
  test_http_client::RequestOptions postHello;
  postHello.method = "POST";
  postHello.target = "/hello";
  postHello.headers.push_back({"Content-Length", "0"});
  auto resp2Opt = test_http_client::request(server.port(), postHello);
  ASSERT_TRUE(resp2Opt.has_value());
  const std::string& resp2 = *resp2Opt;
  EXPECT_NE(resp2.find("405 Method Not Allowed"), std::string::npos);
  test_http_client::RequestOptions getMissing;
  getMissing.method = "GET";
  getMissing.target = "/missing";
  auto resp3Opt = test_http_client::request(server.port(), getMissing);
  ASSERT_TRUE(resp3Opt.has_value());
  const std::string& resp3 = *resp3Opt;
  EXPECT_NE(resp3.find("404 Not Found"), std::string::npos);
  test_http_client::RequestOptions postMulti;
  postMulti.method = "POST";
  postMulti.target = "/multi";
  postMulti.headers.push_back({"Content-Length", "0"});
  auto resp4Opt = test_http_client::request(server.port(), postMulti);
  ASSERT_TRUE(resp4Opt.has_value());
  const std::string& resp4 = *resp4Opt;
  EXPECT_NE(resp4.find("200 OK"), std::string::npos);
  EXPECT_NE(resp4.find("POST!"), std::string::npos);

  done.store(true);
}

TEST(HttpRouting, GlobalFallbackWithPathHandlers) {
  ServerConfig cfg;
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
