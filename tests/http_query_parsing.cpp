// Tests for raw query string exposure and basic percent-decoding integration.
#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>  // std::chrono::milliseconds
#include <string>  // std::string
#include <thread>  // std::jthread, sleep_for

#include "aeronet/http-method.hpp"         // aeronet::http::Method
#include "aeronet/http-request.hpp"        // aeronet::HttpRequest
#include "aeronet/http-response.hpp"       // aeronet::HttpResponse
#include "aeronet/http-server-config.hpp"  // aeronet::HttpServerConfig
#include "aeronet/http-server.hpp"         // aeronet::HttpServer

// use explicit std::chrono::milliseconds below (no using directives)

// simpleGet now provided in aeronet::test_util
#include "aeronet/test_util.hpp"

TEST(HttpQueryParsing, NoQuery) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.addPathHandler("/plain", aeronet::http::Method::GET, [](const aeronet::HttpRequest& req) {
    EXPECT_EQ(req.path(), "/plain");
    EXPECT_EQ(req.queryParams().begin(), req.queryParams().end());
    aeronet::HttpResponse resp;
    resp.statusCode(200).reason("OK").body("NOQ").contentType("text/plain");
    return resp;
  });
  std::jthread thr([&] { server.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto resp = aeronet::test::simpleGet(server.port(), "/plain");
  EXPECT_NE(resp.find("NOQ"), std::string::npos);
  server.stop();
}

TEST(HttpQueryParsing, SimpleQuery) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.addPathHandler("/p", aeronet::http::Method::GET, [](const aeronet::HttpRequest& req) {
    EXPECT_EQ(req.path(), "/p");

    std::string body;
    for (const auto& [key, val] : req.queryParams()) {
      if (!body.empty()) {
        body.push_back('&');
      }
      body.append(key);
      body.push_back('=');
      body.append(val);
    }

    aeronet::HttpResponse resp(200, "OK");
    resp.body(body).contentType("text/plain");
    return resp;
  });
  std::jthread thr([&] { server.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto resp = aeronet::test::simpleGet(server.port(), "/p?a=1&b=2");
  EXPECT_NE(resp.find("a=1&b=2"), std::string::npos);
  server.stop();
}

TEST(HttpQueryParsing, PercentDecodedQuery) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.addPathHandler("/d", aeronet::http::Method::GET, [](const aeronet::HttpRequest& req) -> aeronet::HttpResponse {
    // Query is now fully percent-decoded by parser.
    EXPECT_EQ(req.path(), "/d");
    auto range = req.queryParams();
    auto it = range.begin();
    EXPECT_NE(it, range.end());
    EXPECT_EQ((*it).key, "x");
    EXPECT_EQ((*it).value, "one two");  // %20 decoded
    ++it;
    EXPECT_NE(it, range.end());
    EXPECT_EQ((*it).key, "y");
    EXPECT_EQ((*it).value, "/path");  // %2F decoded
    ++it;
    EXPECT_EQ(it, range.end());
    aeronet::HttpResponse resp;
    // Echo decoded query back in body for client-side verification
    std::string body;
    for (const auto& [key, val] : req.queryParams()) {
      if (!body.empty()) {
        body.push_back('&');
      }
      body.append(key);
      body.push_back('=');
      body.append(val);
    }
    resp.statusCode(200).reason("OK").body(body).contentType("text/plain");
    return resp;
  });
  std::jthread thr([&] { server.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto resp = aeronet::test::simpleGet(server.port(), "/d?x=one%20two&y=%2Fpath");
  // Body should contain decoded query string now.
  EXPECT_NE(resp.find("x=one two&y=/path"), std::string::npos);
  server.stop();
}

TEST(HttpQueryParsing, EmptyQueryAndTrailingQMark) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.addPathHandler("/t", aeronet::http::Method::GET, [](const aeronet::HttpRequest& req) {
    EXPECT_EQ(req.path(), "/t");
    // "?" with nothing after -> empty query view
    EXPECT_EQ(req.queryParams().begin(), req.queryParams().end());
    aeronet::HttpResponse resp;
    resp.statusCode(200).reason("OK").body("EMPTY").contentType("text/plain");
    return resp;
  });
  std::jthread thr([&] { server.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto resp = aeronet::test::simpleGet(server.port(), "/t?");
  EXPECT_NE(resp.find("EMPTY"), std::string::npos);
  server.stop();
}
