#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/rate-limit-middleware.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

namespace {

aeronet::test::TestServer ts(HttpServerConfig{});

auto okHandler() {
  return [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK, "ok"); };
}

}  // namespace

TEST(HttpRateLimit, GlobalPeerAddressLimiterReturns429AndRetryAfter) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/rl", okHandler());

  RateLimitRequestMiddlewareBuilder opts;
  opts.config.requestsPerSecond = 1;
  opts.config.burst = 1;
  opts.keyStrategy = RateLimitClientKeyStrategy::PeerAddress;
  router.addRequestMiddleware(std::move(opts).build());

  const auto r1 = aeronet::test::simpleGet(ts.port(), "/rl");
  EXPECT_TRUE(r1.contains("HTTP/1.1 200")) << r1;

  const auto r2 = aeronet::test::simpleGet(ts.port(), "/rl");
  EXPECT_TRUE(r2.contains("HTTP/1.1 429")) << r2;
  EXPECT_TRUE(r2.contains("Retry-After:")) << r2;
}

TEST(HttpRateLimit, RouteSpecificLimiterOnlyAffectsItsRoute) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/strict", okHandler()).before([] {
    RateLimitRequestMiddlewareBuilder options;
    options.config.requestsPerSecond = 1;
    options.config.burst = 1;
    return std::move(options).build();
  }());
  router.setPath(http::Method::GET, "/open", okHandler());

  const auto strict1 = aeronet::test::simpleGet(ts.port(), "/strict");
  const auto strict2 = aeronet::test::simpleGet(ts.port(), "/strict");
  const auto open1 = aeronet::test::simpleGet(ts.port(), "/open");
  const auto open2 = aeronet::test::simpleGet(ts.port(), "/open");

  EXPECT_TRUE(strict1.contains("HTTP/1.1 200")) << strict1;
  EXPECT_TRUE(strict2.contains("HTTP/1.1 429")) << strict2;
  EXPECT_TRUE(open1.contains("HTTP/1.1 200")) << open1;
  EXPECT_TRUE(open2.contains("HTTP/1.1 200")) << open2;
}

TEST(HttpRateLimit, ForwardedForStrategyUsesHeaderKey) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/xff", okHandler());

  router.addRequestMiddleware(
      RateLimitRequestMiddlewareBuilder{.config = RateLimitConfig{.requestsPerSecond = 1, .burst = 1},
                                        .keyStrategy = RateLimitClientKeyStrategy::XForwardedForFirst}
          .build());

  aeronet::test::RequestOptions req;
  req.method = "GET";
  req.target = "/xff";
  req.headers.emplace_back("X-Forwarded-For", "203.0.113.7");

  const auto r1 = aeronet::test::requestOrThrow(ts.port(), req);
  const auto r2 = aeronet::test::requestOrThrow(ts.port(), req);

  EXPECT_TRUE(r1.contains("HTTP/1.1 200")) << r1;
  EXPECT_TRUE(r2.contains("HTTP/1.1 429")) << r2;
}
