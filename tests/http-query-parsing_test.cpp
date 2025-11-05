#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

namespace {
test::TestServer ts{HttpServerConfig{}};
}

TEST(HttpQueryParsing, NoQuery) {
  ts.server.router().setPath(http::Method::GET, "/plain", [](const HttpRequest& req) {
    EXPECT_EQ(req.path(), "/plain");
    EXPECT_EQ(req.queryParams().begin(), req.queryParams().end());
    HttpResponse resp;
    resp.status(200).reason("OK").body("NOQ");
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/plain");
  EXPECT_TRUE(resp.contains("NOQ"));
}

TEST(HttpQueryParsing, SimpleQuery) {
  ts.server.router().setPath(http::Method::GET, "/p", [](const HttpRequest& req) {
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

    HttpResponse resp(200, "OK");
    resp.body(body);
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/p?a=1&b=2");
  EXPECT_TRUE(resp.contains("a=1&b=2"));
}

TEST(HttpQueryParsing, PercentDecodedQuery) {
  ts.server.router().setPath(http::Method::GET, "/d", [](const HttpRequest& req) -> HttpResponse {
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
    HttpResponse resp;
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
    resp.status(200).reason("OK").body(body);
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/d?x=one%20two&y=%2Fpath");
  // Body should contain decoded query string now.
  EXPECT_TRUE(resp.contains("x=one two&y=/path"));
}

TEST(HttpQueryParsing, EmptyQueryAndTrailingQMark) {
  ts.server.router().setPath(http::Method::GET, "/t", [](const HttpRequest& req) {
    EXPECT_EQ(req.path(), "/t");
    // "?" with nothing after -> empty query view
    EXPECT_EQ(req.queryParams().begin(), req.queryParams().end());
    HttpResponse resp;
    resp.status(200).reason("OK").body("EMPTY");
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/t?");
  EXPECT_TRUE(resp.contains("EMPTY"));
}

TEST(HttpQueryParsingEdge, IncompleteEscapeAtEndShouldBeAccepted) {
  ts.server.router().setPath(http::Method::GET, "/e", [](const HttpRequest& req) -> HttpResponse {
    EXPECT_EQ(req.path(), "/e");
    // "%" at end remains literal
    // Malformed escape -> fallback leaves query raw
    auto it = req.queryParams().begin();
    EXPECT_NE(it, req.queryParams().end());
    EXPECT_EQ((*it).key, "x");
    EXPECT_EQ((*it).value, "%");
    HttpResponse resp(200, "OK");
    resp.body("EDGE1");
    return resp;
  });
  std::string out = test::simpleGet(ts.port(), "/e?x=%");
  EXPECT_TRUE(out.contains("200 OK"));
}

TEST(HttpQueryParsingEdge, IncompleteEscapeOneHexShouldBeAccepted) {
  ts.server.router().setPath(http::Method::GET, "/e2", [](const HttpRequest& req) -> HttpResponse {
    auto it = req.queryParams().begin();
    EXPECT_NE(it, req.queryParams().end());
    EXPECT_EQ((*it).key, "a");
    // Invalid -> left as literal
    EXPECT_EQ((*it).value, "%A");
    HttpResponse resp(200, "OK");
    resp.body("EDGE2");
    return resp;
  });
  std::string resp = test::simpleGet(ts.port(), "/e2?a=%A");

  EXPECT_TRUE(resp.contains("200 OK"));
}

TEST(HttpQueryParsingEdge, MultiplePairsAndEmptyValue) {
  ts.server.router().setPath(http::Method::GET, "/m", [](const HttpRequest& req) -> HttpResponse {
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
    HttpResponse resp(200, "OK");
    resp.body("EDGE3");
    return resp;
  });
  std::string resp = test::simpleGet(ts.port(), "/m?k=1&empty=&novalue");
  EXPECT_TRUE(resp.contains("EDGE3"));
}

TEST(HttpQueryParsingEdge, PercentDecodingKeyAndValue) {
  ts.server.router().setPath(http::Method::GET, "/pd", [](const HttpRequest& req) -> HttpResponse {
    // encoded: %66 -> 'f'
    // Fully decodable -> parser decodes now
    auto it = req.queryParams().begin();
    EXPECT_NE(it, req.queryParams().end());
    EXPECT_EQ((*it).key, "fo");
    EXPECT_EQ((*it).value, "bar baz");
    HttpResponse resp;
    resp.status(200).reason("OK").body("EDGE4");
    return resp;
  });
  std::string resp = test::simpleGet(ts.port(), "/pd?%66o=bar%20baz");

  EXPECT_TRUE(resp.contains("EDGE4"));
}

TEST(HttpQueryStructuredBindings, IterateKeyValues) {
  ts.server.router().setPath(http::Method::GET, "/sb", [](const HttpRequest& req) -> HttpResponse {
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
    HttpResponse resp(200, "OK");
    resp.body("OK");
    return resp;
  });
  // Build raw HTTP request using helpers
  test::ClientConnection client(ts.port());
  std::string req = "GET /sb?a=1&b=two%20words&empty=&novalue HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
  test::sendAll(client.fd(), req);
  auto resp = test::recvUntilClosed(client.fd());
  EXPECT_TRUE(resp.contains("OK"));
}
