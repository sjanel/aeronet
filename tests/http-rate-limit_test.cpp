#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

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

class ThrowingRateLimitStore final : public IRateLimitStore {
 public:
  RateLimitDecision consume([[maybe_unused]] std::string_view key,
                            [[maybe_unused]] std::chrono::steady_clock::time_point now,
                            [[maybe_unused]] const RateLimitConfig& config) override {
    throw std::runtime_error("store failure");
  }
};

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

  router.addRequestMiddleware(RateLimitRequestMiddlewareBuilder{
      .config = RateLimitConfig{.requestsPerSecond = 1, .burst = 1},
      .store = {},
      .keyStrategy = RateLimitClientKeyStrategy::XForwardedForFirst,
      .customKeyExtractor = {}}.build());

  aeronet::test::RequestOptions req;
  req.method = "GET";
  req.target = "/xff";
  req.headers.emplace_back("X-Forwarded-For", "203.0.113.7");

  const auto r1 = aeronet::test::requestOrThrow(ts.port(), req);
  const auto r2 = aeronet::test::requestOrThrow(ts.port(), req);

  EXPECT_TRUE(r1.contains("HTTP/1.1 200")) << r1;
  EXPECT_TRUE(r2.contains("HTTP/1.1 429")) << r2;
}

TEST(HttpRateLimit, ForwardedForStrategyBypassesWhenHeaderMissing) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/xff-missing", okHandler());

  RateLimitRequestMiddlewareBuilder options;
  options.config.requestsPerSecond = 1;
  options.config.burst = 1;
  options.keyStrategy = RateLimitClientKeyStrategy::XForwardedForFirst;
  router.addRequestMiddleware(std::move(options).build());

  const auto r1 = aeronet::test::simpleGet(ts.port(), "/xff-missing");
  const auto r2 = aeronet::test::simpleGet(ts.port(), "/xff-missing");

  EXPECT_TRUE(r1.contains("HTTP/1.1 200")) << r1;
  EXPECT_TRUE(r2.contains("HTTP/1.1 200")) << r2;
}

TEST(HttpRateLimit, HeaderValueStrategyUsesTrimmedHeaderAndConstBuild) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/header-key", okHandler());

  RateLimitRequestMiddlewareBuilder options;
  options.config.requestsPerSecond = 1;
  options.config.burst = 1;
  options.keyStrategy = RateLimitClientKeyStrategy::HeaderValue;
  options.headerName = "x-api-key";
  router.addRequestMiddleware(options.build());

  aeronet::test::RequestOptions req;
  req.method = "GET";
  req.target = "/header-key";
  req.headers.emplace_back("X-Api-Key", "  client-a  ");

  const auto r1 = aeronet::test::requestOrThrow(ts.port(), req);
  const auto r2 = aeronet::test::requestOrThrow(ts.port(), req);

  EXPECT_TRUE(r1.contains("HTTP/1.1 200")) << r1;
  EXPECT_TRUE(r2.contains("HTTP/1.1 429")) << r2;
}

TEST(HttpRateLimit, HeaderValueStrategyRequiresHeaderName) {
  RateLimitRequestMiddlewareBuilder options;
  options.keyStrategy = RateLimitClientKeyStrategy::HeaderValue;
  options.headerName = {};

  EXPECT_THROW(static_cast<void>(options.build()), std::invalid_argument);
}

TEST(HttpRateLimit, CustomStrategyWithoutExtractorBypassesRequests) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/custom-none", okHandler());

  RateLimitRequestMiddlewareBuilder options;
  options.config.requestsPerSecond = 1;
  options.config.burst = 1;
  options.keyStrategy = RateLimitClientKeyStrategy::Custom;
  router.addRequestMiddleware(std::move(options).build());

  const auto r1 = aeronet::test::simpleGet(ts.port(), "/custom-none");
  const auto r2 = aeronet::test::simpleGet(ts.port(), "/custom-none");

  EXPECT_TRUE(r1.contains("HTTP/1.1 200")) << r1;
  EXPECT_TRUE(r2.contains("HTTP/1.1 200")) << r2;
}

TEST(HttpRateLimit, CustomStrategyUsesExtractorKey) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/custom-key", okHandler());

  RateLimitRequestMiddlewareBuilder options;
  options.config.requestsPerSecond = 1;
  options.config.burst = 1;
  options.keyStrategy = RateLimitClientKeyStrategy::Custom;
  options.customKeyExtractor = [](const HttpRequest& request) { return request.headerValueOrEmpty("x-client-id"); };
  router.addRequestMiddleware(std::move(options).build());

  aeronet::test::RequestOptions req;
  req.method = "GET";
  req.target = "/custom-key";
  req.headers.emplace_back("X-Client-Id", "tenant-a");

  const auto r1 = aeronet::test::requestOrThrow(ts.port(), req);
  const auto r2 = aeronet::test::requestOrThrow(ts.port(), req);

  EXPECT_TRUE(r1.contains("HTTP/1.1 200")) << r1;
  EXPECT_TRUE(r2.contains("HTTP/1.1 429")) << r2;
}

TEST(HttpRateLimit, StoreExceptionsFailOpenWhenConfigured) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/throw-open", okHandler());

  RateLimitRequestMiddlewareBuilder options;
  options.config.failOpen = true;
  options.store = std::make_shared<ThrowingRateLimitStore>();
  router.addRequestMiddleware(std::move(options).build());

  const auto response = aeronet::test::simpleGet(ts.port(), "/throw-open");
  EXPECT_TRUE(response.contains("HTTP/1.1 200")) << response;
}

TEST(HttpRateLimit, StoreExceptionsFailClosedWithRetryAfterAndBody) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/throw-closed", okHandler());

  RateLimitRequestMiddlewareBuilder options;
  options.config.failOpen = false;
  options.store = std::make_shared<ThrowingRateLimitStore>();
  options.rejectionBody = "slow down";
  router.addRequestMiddleware(std::move(options).build());

  const auto response = aeronet::test::simpleGet(ts.port(), "/throw-closed");
  EXPECT_TRUE(response.contains("HTTP/1.1 429")) << response;
  EXPECT_TRUE(response.contains("Retry-After: 1")) << response;
  EXPECT_TRUE(response.contains("slow down")) << response;
}
