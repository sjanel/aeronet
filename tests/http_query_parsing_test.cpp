#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_util.hpp"

TEST(HttpQueryParsing, NoQuery) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.router().setPath("/plain", aeronet::http::Method::GET, [](const aeronet::HttpRequest& req) {
    EXPECT_EQ(req.path(), "/plain");
    EXPECT_EQ(req.queryParams().begin(), req.queryParams().end());
    aeronet::HttpResponse resp;
    resp.statusCode(200).reason("OK").body("NOQ").contentType("text/plain");
    return resp;
  });
  std::jthread thr([&] { server.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto resp = aeronet::test::simpleGet(server.port(), "/plain");
  EXPECT_TRUE(resp.contains("NOQ"));
  server.stop();
}

TEST(HttpQueryParsing, SimpleQuery) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.router().setPath("/p", aeronet::http::Method::GET, [](const aeronet::HttpRequest& req) {
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
  EXPECT_TRUE(resp.contains("a=1&b=2"));
  server.stop();
}

TEST(HttpQueryParsing, PercentDecodedQuery) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.router().setPath("/d", aeronet::http::Method::GET,
                          [](const aeronet::HttpRequest& req) -> aeronet::HttpResponse {
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
  EXPECT_TRUE(resp.contains("x=one two&y=/path"));
  server.stop();
}

TEST(HttpQueryParsing, EmptyQueryAndTrailingQMark) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.router().setPath("/t", aeronet::http::Method::GET, [](const aeronet::HttpRequest& req) {
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
  EXPECT_TRUE(resp.contains("EMPTY"));
  server.stop();
}

TEST(HttpQueryParsingEdge, IncompleteEscapeAtEndShouldBeAccepted) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.router().setPath("/e", aeronet::http::Method::GET,
                          [](const aeronet::HttpRequest& req) -> aeronet::HttpResponse {
                            EXPECT_EQ(req.path(), "/e");
                            // "%" at end remains literal
                            // Malformed escape -> fallback leaves query raw
                            auto it = req.queryParams().begin();
                            EXPECT_NE(it, req.queryParams().end());
                            EXPECT_EQ((*it).key, "x");
                            EXPECT_EQ((*it).value, "%");
                            aeronet::HttpResponse resp(200, "OK");
                            resp.body("EDGE1").contentType("text/plain");
                            return resp;
                          });
  std::jthread thr([&] { server.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string out = aeronet::test::simpleGet(server.port(), "/e?x=%");
  EXPECT_TRUE(out.contains("200 OK"));
  server.stop();
}

TEST(HttpQueryParsingEdge, IncompleteEscapeOneHexShouldBeAccepted) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.router().setPath("/e2", aeronet::http::Method::GET,
                          [](const aeronet::HttpRequest& req) -> aeronet::HttpResponse {
                            auto it = req.queryParams().begin();
                            EXPECT_NE(it, req.queryParams().end());
                            EXPECT_EQ((*it).key, "a");
                            // Invalid -> left as literal
                            EXPECT_EQ((*it).value, "%A");
                            aeronet::HttpResponse resp(200, "OK");
                            resp.body("EDGE2").contentType("text/plain");
                            return resp;
                          });
  std::jthread thr([&] { server.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string resp = aeronet::test::simpleGet(server.port(), "/e2?a=%A");

  EXPECT_TRUE(resp.contains("200 OK"));
  server.stop();
}

TEST(HttpQueryParsingEdge, MultiplePairsAndEmptyValue) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.router().setPath("/m", aeronet::http::Method::GET,
                          [](const aeronet::HttpRequest& req) -> aeronet::HttpResponse {
                            auto range = req.queryParams();
                            auto it = range.begin();
                            EXPECT_NE(it, range.end());
                            EXPECT_EQ((*it).key, "k");
                            EXPECT_EQ((*it).value, "1");
                            ++it;
                            EXPECT_NE(it, range.end());
                            EXPECT_EQ((*it).key, "empty");
                            EXPECT_EQ((*it).value, "");
                            ++it;
                            EXPECT_NE(it, range.end());
                            EXPECT_EQ((*it).key, "novalue");
                            EXPECT_EQ((*it).value, "");
                            ++it;
                            EXPECT_EQ(it, range.end());
                            aeronet::HttpResponse resp(200, "OK");
                            resp.body("EDGE3").contentType("text/plain");
                            return resp;
                          });
  std::jthread thr([&] { server.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string resp = aeronet::test::simpleGet(server.port(), "/m?k=1&empty=&novalue");
  EXPECT_TRUE(resp.contains("EDGE3"));
  server.stop();
}

TEST(HttpQueryParsingEdge, PercentDecodingKeyAndValue) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.router().setPath("/pd", aeronet::http::Method::GET,
                          [](const aeronet::HttpRequest& req) -> aeronet::HttpResponse {
                            // encoded: %66 -> 'f'
                            // Fully decodable -> parser decodes now
                            auto it = req.queryParams().begin();
                            EXPECT_NE(it, req.queryParams().end());
                            EXPECT_EQ((*it).key, "fo");
                            EXPECT_EQ((*it).value, "bar baz");
                            aeronet::HttpResponse resp;
                            resp.statusCode(200).reason("OK").body("EDGE4").contentType("text/plain");
                            return resp;
                          });
  std::jthread thr([&] { server.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string resp = aeronet::test::simpleGet(server.port(), "/pd?%66o=bar%20baz");

  EXPECT_TRUE(resp.contains("EDGE4"));
  server.stop();
}

TEST(HttpQueryStructuredBindings, IterateKeyValues) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.router().setPath("/sb", aeronet::http::Method::GET,
                          [](const aeronet::HttpRequest& req) -> aeronet::HttpResponse {
                            EXPECT_EQ(req.path(), "/sb");
                            int count = 0;
                            bool sawA = false;
                            bool sawB = false;
                            bool sawEmpty = false;
                            bool sawNoValue = false;
                            for (const auto& [k, v] : req.queryParams()) {
                              ++count;
                              if (k == "a") {
                                EXPECT_EQ(v, "1");
                                sawA = true;
                              } else if (k == "b") {
                                EXPECT_EQ(v, "two words");
                                sawB = true;
                              } else if (k == "empty") {
                                EXPECT_TRUE(v.empty());
                                sawEmpty = true;
                              } else if (k == "novalue") {
                                EXPECT_TRUE(v.empty());
                                sawNoValue = true;
                              }
                            }
                            EXPECT_EQ(count, 4);
                            EXPECT_TRUE(sawA && sawB && sawEmpty && sawNoValue);
                            aeronet::HttpResponse resp(200, "OK");
                            resp.body("OK").contentType("text/plain");
                            return resp;
                          });
  std::jthread thr([&] { server.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  // Build raw HTTP request using helpers
  aeronet::test::ClientConnection client(server.port());
  std::string req = "GET /sb?a=1&b=two%20words&empty=&novalue HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(client.fd(), req));
  auto resp = aeronet::test::recvUntilClosed(client.fd());
  EXPECT_TRUE(resp.contains("OK"));
  server.stop();
}
