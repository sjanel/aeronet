// IWYU: add direct includes for symbols used in tests
#include <gtest/gtest.h>

#include <chrono>  // chrono literals
#include <string>  // std::string
#include <thread>  // std::jthread, sleep_for

#include "aeronet/http-method.hpp"         // aeronet::http::Method
#include "aeronet/http-request.hpp"        // aeronet::HttpRequest
#include "aeronet/http-response.hpp"       // aeronet::HttpResponse
#include "aeronet/http-server-config.hpp"  // aeronet::HttpServerConfig
#include "aeronet/http-server.hpp"         // aeronet::HttpServer
#include "test_raw_get.hpp"

using namespace std::chrono_literals;

TEST(HttpQueryParsingEdge, IncompleteEscapeAtEndShouldBeAccepted) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.addPathHandler("/e", aeronet::http::Method::GET, [](const aeronet::HttpRequest& req) -> aeronet::HttpResponse {
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
  std::string out;
  test_helpers::rawGet(server.port(), "/e?x=%", out);
  EXPECT_NE(out.find("200 OK"), std::string::npos);
  server.stop();
}

TEST(HttpQueryParsingEdge, IncompleteEscapeOneHexShouldBeAccepted) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.addPathHandler("/e2", aeronet::http::Method::GET,
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
  std::string resp;
  test_helpers::rawGet(server.port(), "/e2?a=%A", resp);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  server.stop();
}

TEST(HttpQueryParsingEdge, MultiplePairsAndEmptyValue) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.addPathHandler("/m", aeronet::http::Method::GET, [](const aeronet::HttpRequest& req) -> aeronet::HttpResponse {
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
  std::string resp;
  test_helpers::rawGet(server.port(), "/m?k=1&empty=&novalue", resp);
  EXPECT_NE(resp.find("EDGE3"), std::string::npos);
  server.stop();
}

TEST(HttpQueryParsingEdge, PercentDecodingKeyAndValue) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.addPathHandler("/pd", aeronet::http::Method::GET,
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
  std::string resp;
  test_helpers::rawGet(server.port(), "/pd?%66o=bar%20baz", resp);
  EXPECT_NE(resp.find("EDGE4"), std::string::npos);
  server.stop();
}
