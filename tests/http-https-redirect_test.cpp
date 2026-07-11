#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/request-metrics.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

namespace {

HttpServerConfig RedirectConfig(uint16_t targetPort = 443,
                                http::StatusCode statusCode = http::StatusCodeMovedPermanently) {
  return HttpServerConfig{}.withHttpsRedirect(targetPort, statusCode);
}

}  // namespace

TEST(HttpsRedirectIntegration, BasicGetRedirectStandardPort) {
  test::TestServer ts(RedirectConfig());
  // A handler that must NOT be reached (redirect bypasses routing).
  ts.router().setPath(http::Method::GET, "/path",
                      [](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK, "handler"); });

  test::RequestOptions opt;
  opt.target = "/path";
  opt.host = "example.com:80";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(ts.port(), opt));

  EXPECT_EQ(resp.statusCode, http::StatusCodeMovedPermanently);
  EXPECT_EQ(test::getHeader(resp, "location"), "https://example.com/path");
  EXPECT_EQ(test::toLower(test::getHeader(resp, "connection")), "close");
}

TEST(HttpsRedirectIntegration, NonStandardPortAppended) {
  test::TestServer ts(RedirectConfig(8443));

  test::RequestOptions opt;
  opt.target = "/secure";
  opt.host = "example.com";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(ts.port(), opt));

  EXPECT_EQ(resp.statusCode, http::StatusCodeMovedPermanently);
  EXPECT_EQ(test::getHeader(resp, "location"), "https://example.com:8443/secure");
}

TEST(HttpsRedirectIntegration, PreservesQueryString) {
  test::TestServer ts(RedirectConfig());

  test::RequestOptions opt;
  opt.target = "/search?q=foo&lang=en";
  opt.host = "example.com";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(ts.port(), opt));

  EXPECT_EQ(resp.statusCode, http::StatusCodeMovedPermanently);
  EXPECT_EQ(test::getHeader(resp, "location"), "https://example.com/search?q=foo&lang=en");
}

TEST(HttpsRedirectIntegration, CustomStatusCode308) {
  test::TestServer ts(RedirectConfig(443, http::StatusCodePermanentRedirect));

  test::RequestOptions opt;
  opt.target = "/";
  opt.host = "host.test";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(ts.port(), opt));

  EXPECT_EQ(resp.statusCode, http::StatusCodePermanentRedirect);
  EXPECT_EQ(test::getHeader(resp, "location"), "https://host.test/");
}

TEST(HttpsRedirectIntegration, PostWithBodyIsRedirected) {
  test::TestServer ts(RedirectConfig());

  test::RequestOptions opt;
  opt.method = "POST";
  opt.target = "/submit";
  opt.host = "example.com";
  opt.body = "payload=1234";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(ts.port(), opt));

  EXPECT_EQ(resp.statusCode, http::StatusCodeMovedPermanently);
  EXPECT_EQ(test::getHeader(resp, "location"), "https://example.com/submit");
}

TEST(HttpsRedirectIntegration, MissingHostReturns400) {
  test::TestServer ts(RedirectConfig());

  // HTTP/1.0 request without a Host header: no absolute https URL can be built -> 400.
  const std::string raw = "GET /x HTTP/1.0\r\n\r\n";
  auto resp = test::parseResponseOrThrow(test::sendAndCollect(ts.port(), raw));

  EXPECT_EQ(resp.statusCode, http::StatusCodeBadRequest);
}

TEST(HttpsRedirectIntegration, IPv6HostPreservedWithBrackets) {
  test::TestServer ts(RedirectConfig(8443));

  test::RequestOptions opt;
  opt.target = "/a";
  opt.host = "[::1]:80";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(ts.port(), opt));

  EXPECT_EQ(resp.statusCode, http::StatusCodeMovedPermanently);
  EXPECT_EQ(test::getHeader(resp, "location"), "https://[::1]:8443/a");
}

TEST(HttpsRedirectIntegration, HeadRequestRedirectedWithoutBody) {
  test::TestServer ts(RedirectConfig());

  test::RequestOptions opt;
  opt.method = "HEAD";
  opt.target = "/x";
  opt.host = "example.com";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(ts.port(), opt));

  EXPECT_EQ(resp.statusCode, http::StatusCodeMovedPermanently);
  EXPECT_EQ(test::getHeader(resp, "location"), "https://example.com/x");
  EXPECT_TRUE(resp.body.empty());
}

TEST(HttpsRedirectIntegration, EmitsRequestMetrics) {
  test::TestServer ts(RedirectConfig());
  std::atomic<http::StatusCode> seenStatus{0};
  std::atomic<int> count{0};
  ts.server.setMetricsCallback([&](const RequestMetrics& requestMetrics) {
    seenStatus.store(requestMetrics.status);
    count.fetch_add(1);
  });

  test::RequestOptions opt;
  opt.target = "/metrics-path";
  opt.host = "example.com";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(ts.port(), opt));

  EXPECT_EQ(resp.statusCode, http::StatusCodeMovedPermanently);
  EXPECT_EQ(count.load(), 1);
  EXPECT_EQ(seenStatus.load(), http::StatusCodeMovedPermanently);
}
