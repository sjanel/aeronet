#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

namespace {
test::TestServer ts(HttpServerConfig{});
}

TEST(HttpUrlDecoding, SpaceDecoding) {
  ts.server.router().setPath(http::Method::GET, "/hello world", [](const HttpRequest &req) {
    return HttpResponse(http::StatusCodeOK).reason("OK").body(std::string(req.path()));
  });
  test::RequestOptions optHello;
  optHello.method = "GET";
  optHello.target = "/hello%20world";
  auto respOwned = test::requestOrThrow(ts.server.port(), optHello);
  std::string_view resp = respOwned;
  EXPECT_TRUE(resp.contains("200 OK"));
  EXPECT_TRUE(resp.contains("hello world"));
}

TEST(HttpUrlDecoding, Utf8Decoded) {
  // Path contains snowman + space + 'x'
  std::string decodedPath = "/\xE2\x98\x83 x";  // /☃ x
  ts.server.router().setPath(http::Method::GET, decodedPath,
                             [](const HttpRequest &) { return HttpResponse(200, "OK").body("utf8"); });
  // Percent-encoded UTF-8 for snowman (E2 98 83) plus %20 and 'x'
  test::RequestOptions optUtf8;
  optUtf8.method = "GET";
  optUtf8.target = "/%E2%98%83%20x";
  auto respOwned = test::requestOrThrow(ts.server.port(), optUtf8);
  std::string_view resp = respOwned;
  EXPECT_TRUE(resp.contains("200 OK"));
  EXPECT_TRUE(resp.contains("utf8"));
}

TEST(HttpUrlDecoding, PlusIsNotSpace) {
  ts.server.router().setPath(http::Method::GET, "/a+b",
                             [](const HttpRequest &) { return HttpResponse(200, "OK").body("plus"); });
  test::RequestOptions optPlus;
  optPlus.method = "GET";
  optPlus.target = "/a+b";
  auto respOwned = test::requestOrThrow(ts.server.port(), optPlus);
  std::string_view resp = respOwned;
  EXPECT_TRUE(resp.contains("200 OK"));
  EXPECT_TRUE(resp.contains("plus"));
}

TEST(HttpUrlDecoding, InvalidPercentSequence400) {
  test::RequestOptions optBad;
  optBad.method = "GET";
  optBad.target = "/bad%G1";
  auto respOwned = test::requestOrThrow(ts.server.port(), optBad);
  std::string_view resp = respOwned;
  EXPECT_TRUE(resp.contains("400 Bad Request"));
}

TEST(HttpUrlDecoding, IncompletePercentSequence400) {
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/bad%";
  auto resp = test::requestOrThrow(ts.server.port(), opt);
  EXPECT_TRUE(resp.contains("400 Bad Request"));
}

TEST(HttpUrlDecoding, MixedSegmentsDecoding) {
  ts.server.router().setPath(http::Method::GET, "/seg one/part%/two",
                             [](const HttpRequest &req) { return HttpResponse(200, "OK").body(req.path()); });
  // encodes space in first segment only
  test::RequestOptions opt2;
  opt2.method = "GET";
  opt2.target = "/seg%20one/part%25/two";
  auto resp = test::requestOrThrow(ts.server.port(), opt2);
  EXPECT_TRUE(resp.contains("200 OK"));
  EXPECT_TRUE(resp.contains("/seg one/part%/two"));
}
