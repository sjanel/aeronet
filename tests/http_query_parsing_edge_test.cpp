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

using namespace std::chrono_literals;

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
