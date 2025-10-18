#include <gtest/gtest.h>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

TEST(HttpOptionsTrace, OptionsStarReturnsAllow) {
  HttpServerConfig cfg{};
  // Install global simple handler so Router.allowedMethods returns non-empty set
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setDefault([](const HttpRequest&) { return HttpResponse(200); });

  auto resp = aeronet::test::requestOrThrow(
      ts.port(), aeronet::test::RequestOptions{.method = "OPTIONS", .target = "*", .body = "", .headers = {}});
  ASSERT_TRUE(resp.contains("200"));
  ASSERT_TRUE(resp.contains("Allow:"));
}

TEST(HttpOptionsTrace, TraceEchoWhenEnabled) {
  HttpServerConfig cfg{};
  cfg.withTracePolicy(HttpServerConfig::TraceMethodPolicy::EnabledPlainAndTLS);
  aeronet::test::TestServer ts(cfg);

  auto resp = aeronet::test::requestOrThrow(
      ts.port(), aeronet::test::RequestOptions{.method = "TRACE", .target = "/test", .body = "abcd", .headers = {}});
  // Should contain echoed body
  ASSERT_TRUE(resp.contains("abcd"));
  ASSERT_TRUE(resp.contains("Content-Type: message/http"));
}

TEST(HttpOptionsTrace, TraceDisabledReturns405) {
  HttpServerConfig cfg{};
  aeronet::test::TestServer ts(cfg);

  auto resp = aeronet::test::requestOrThrow(
      ts.port(), aeronet::test::RequestOptions{.method = "TRACE", .target = "/test", .body = "", .headers = {}});
  ASSERT_TRUE(resp.contains("405"));
}

TEST(HttpOptionsTrace, TraceEnabledPlainOnlyAllowsPlaintext) {
  // EnabledPlainOnly should still allow TRACE over plaintext
  HttpServerConfig cfg{};
  cfg.withTracePolicy(HttpServerConfig::TraceMethodPolicy::EnabledPlainOnly);
  aeronet::test::TestServer ts(cfg);

  // Send TRACE over plaintext
  auto resp = aeronet::test::requestOrThrow(
      ts.port(), aeronet::test::RequestOptions{.method = "TRACE", .target = "/test", .body = "", .headers = {}});
  ASSERT_TRUE(resp.contains("200"));
}
