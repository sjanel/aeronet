#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

// (Removed raw() helper; using shared aeronet::test::request)

TEST(HttpUrlDecodingExtra, IncompletePercentSequence400) {
  HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(1);
  HttpServer server(cfg);
  std::atomic<bool> done = false;
  std::jthread th([&] { server.runUntil([&] { return done.load(); }); });
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/bad%";
  auto resp = aeronet::test::requestOrThrow(server.port(), opt);
  EXPECT_NE(resp.find("400 Bad Request"), std::string::npos);
  done = true;  // jthread auto-joins on destruction
}

TEST(HttpUrlDecodingExtra, MixedSegmentsDecoding) {
  HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(2);
  HttpServer server(cfg);
  server.router().setPath("/seg one/part%/two", http::Method::GET, [](const HttpRequest &req) {
    return aeronet::HttpResponse(200, "OK").body(req.path()).contentType(aeronet::http::ContentTypeTextPlain);
  });
  std::atomic<bool> done = false;
  std::jthread th([&] { server.runUntil([&] { return done.load(); }); });
  for (int i = 0; i < 200 && (!server.isRunning()); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // encodes space in first segment only
  aeronet::test::RequestOptions opt2;
  opt2.method = "GET";
  opt2.target = "/seg%20one/part%25/two";
  auto resp = aeronet::test::requestOrThrow(server.port(), opt2);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("/seg one/part%/two"), std::string::npos);
  done = true;  // jthread auto-joins on destruction
}
