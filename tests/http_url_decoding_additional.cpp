#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
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

// (Removed raw() helper; using shared test_http_client::request)

TEST(HttpUrlDecodingExtra, IncompletePercentSequence400) {
  HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(1);
  HttpServer server(cfg);
  std::atomic<bool> done = false;
  std::jthread th([&] { server.runUntil([&] { return done.load(); }); });
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  test_http_client::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/bad%";
  auto resp = test_http_client::request_or_throw(server.port(), opt);
  EXPECT_NE(resp.find("400 Bad Request"), std::string::npos);
  done = true;  // jthread auto-joins on destruction
}

TEST(HttpUrlDecodingExtra, MixedSegmentsDecoding) {
  HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(2);
  HttpServer server(cfg);
  http::MethodSet ms{http::Method::GET};
  server.addPathHandler("/seg one/part%/two", ms, [](const HttpRequest &req) {
    return aeronet::HttpResponse(200, "OK").body(req.path()).contentType(aeronet::http::ContentTypeTextPlain);
  });
  std::atomic<bool> done = false;
  std::jthread th([&] { server.runUntil([&] { return done.load(); }); });
  for (int i = 0; i < 200 && (!server.isRunning()); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // encodes space in first segment only
  test_http_client::RequestOptions opt2;
  opt2.method = "GET";
  opt2.target = "/seg%20one/part%25/two";
  auto resp = test_http_client::request_or_throw(server.port(), opt2);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("/seg one/part%/two"), std::string::npos);
  done = true;  // jthread auto-joins on destruction
}
