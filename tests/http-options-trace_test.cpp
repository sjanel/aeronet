#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <utility>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

namespace {
test::TestServer ts(HttpServerConfig{}, RouterConfig{}, std::chrono::milliseconds{5});
auto port = ts.port();
}  // namespace

TEST(HttpOptionsTrace, OptionsStarReturnsAllow) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(200); });

  auto resp =
      test::requestOrThrow(port, test::RequestOptions{.method = "OPTIONS", .target = "*", .body = "", .headers = {}});
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  std::string allow(http::Allow);
  allow += http::HeaderSep;
  ASSERT_TRUE(resp.contains(allow));
}

TEST(HttpOptionsTrace, TraceEchoWhenEnabled) {
  ts.postConfigUpdate(
      [](HttpServerConfig& cfg) { cfg.withTracePolicy(HttpServerConfig::TraceMethodPolicy::EnabledPlainAndTLS); });

  auto resp = test::requestOrThrow(
      port,
      test::RequestOptions{.method = "TRACE", .target = "/test", .body = "", .headers = {{"X-Test-Header", "value"}}});

  ASSERT_FALSE(resp.empty());

  // TRACE response must be message/http
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::ContentType, "message/http")));

  // Should echo request line
  ASSERT_TRUE(resp.contains("TRACE /test HTTP/"));

  // Should echo headers
  ASSERT_TRUE(resp.contains("X-Test-Header: value"));
}

TEST(HttpOptionsTrace, TraceDisabledReturns405) {
  ts.postConfigUpdate(
      [](HttpServerConfig& cfg) { cfg.withTracePolicy(HttpServerConfig::TraceMethodPolicy::Disabled); });

  auto resp =
      test::requestOrThrow(port, test::RequestOptions{.method = "TRACE", .target = "/test", .body = "", .headers = {}});
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 405"));
}

TEST(HttpOptionsTrace, TraceEnabledPlainOnlyAllowsPlaintext) {
  ts.postConfigUpdate(
      [](HttpServerConfig& cfg) { cfg.withTracePolicy(HttpServerConfig::TraceMethodPolicy::EnabledPlainOnly); });

  // Send TRACE over plaintext
  auto resp =
      test::requestOrThrow(port, test::RequestOptions{.method = "TRACE", .target = "/test", .body = "", .headers = {}});
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
}

namespace {
CorsPolicy MakePolicy() {
  CorsPolicy policy;
  policy.allowOrigin("https://app.example")
      .allowMethods(http::Method::GET | http::Method::POST)
      .allowAnyRequestHeaders();
  return policy;
}
}  // namespace

class HttpCorsIntegration : public ::testing::Test {
 public:
  static RouterConfig MakeConfigWithCors() {
    RouterConfig cfg{};
    cfg.withDefaultCorsPolicy(MakePolicy());
    return cfg;
  }

  void SetUp() override { ts.router() = Router{MakeConfigWithCors()}; }
};

TEST_F(HttpCorsIntegration, PreflightUsesRouterAllowedMethods) {
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequest&) { return HttpResponse("ok"); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"},
                 {"Access-Control-Request-Method", "GET"},
                 {"Access-Control-Request-Headers", "X-Trace"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeNoContent);

  auto originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://app.example");

  auto methodsIt = parsed.headers.find(http::AccessControlAllowMethods);
  ASSERT_NE(methodsIt, parsed.headers.end());
  EXPECT_EQ(methodsIt->second, "GET");

  auto hdrsIt = parsed.headers.find(http::AccessControlAllowHeaders);
  ASSERT_NE(hdrsIt, parsed.headers.end());
  EXPECT_EQ(hdrsIt->second, "*");
}

TEST_F(HttpCorsIntegration, PreflightMethodDeniedReturns405WithAllow) {
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"}, {"Access-Control-Request-Method", "PUT"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeMethodNotAllowed);

  auto allowIt = parsed.headers.find(http::Allow);
  ASSERT_NE(allowIt, parsed.headers.end());
  EXPECT_EQ(allowIt->second, "GET");
}

TEST_F(HttpCorsIntegration, PreflightOriginDeniedReturns403) {
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://denied.example"}, {"Access-Control-Request-Method", "GET"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeForbidden);

  EXPECT_FALSE(parsed.headers.contains(http::AccessControlAllowOrigin));
}

TEST_F(HttpCorsIntegration, ActualRequestIncludesAllowOriginHeader) {
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequest&) { return HttpResponse("ok"); });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);

  auto originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://app.example");
}

TEST_F(HttpCorsIntegration, ActualRequestOriginDeniedReturns403) {
  std::atomic<bool> handlerInvoked{false};
  ts.router().setPath(http::Method::GET, "/data", [&handlerInvoked](const HttpRequest&) {
    handlerInvoked.store(true);
    return HttpResponse(http::StatusCodeOK);
  });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://blocked.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeForbidden);
  EXPECT_FALSE(handlerInvoked.load());
}

TEST_F(HttpCorsIntegration, StreamingResponseCarriesCorsHeaders) {
  ts.router().setPath(http::Method::GET, "/stream", [](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody("chunk-one");
    writer.end();
  });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/stream";
  opt.headers = {{"Origin", "https://app.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);

  auto originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://app.example");

  // Verify Vary: Origin is present for mirrored origin (credentials enabled in fixture)
  auto varyIt = parsed.headers.find(http::Vary);
  ASSERT_NE(varyIt, parsed.headers.end());
  EXPECT_TRUE(varyIt->second.contains(http::Origin));

  EXPECT_EQ(parsed.plainBody, "chunk-one");
}

TEST_F(HttpCorsIntegration, StreamingVaryHeaderAppendsOrigin) {
  ts.router().setPath(http::Method::GET, "/stream", [](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.header(http::Vary, http::AcceptEncoding);
    writer.contentType("text/plain");
    writer.writeBody("data");
    writer.end();
  });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/stream";
  opt.headers = {{"Origin", "https://app.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);

  auto varyIt = parsed.headers.find(http::Vary);
  ASSERT_NE(varyIt, parsed.headers.end());
  EXPECT_TRUE(varyIt->second.contains(http::AcceptEncoding));
  EXPECT_TRUE(varyIt->second.contains(http::Origin));
}

TEST_F(HttpCorsIntegration, StreamingOriginDeniedSkipsHandler) {
  std::atomic<bool> handlerInvoked{false};
  ts.router().setPath(http::Method::GET, "/stream", [&handlerInvoked](const HttpRequest&, HttpResponseWriter& writer) {
    handlerInvoked.store(true);
    writer.status(http::StatusCodeOK);
    writer.writeBody("should-not-send");
    writer.end();
  });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/stream";
  opt.headers = {{"Origin", "https://blocked.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeForbidden);
  EXPECT_FALSE(handlerInvoked.load());
  EXPECT_EQ(parsed.headers.count(http::AccessControlAllowOrigin), 0);
}

TEST_F(HttpCorsIntegration, PerRouteCorsPolicyOverridesDefault_ActualAndPreflight) {
  // Attach a per-route policy that only allows https://per.example and GET
  CorsPolicy per;
  per.allowOrigin("https://per.example").allowMethods(http::Method::GET).allowAnyRequestHeaders();

  ts.router()
      .setPath(http::Method::GET, "/per", [](const HttpRequest&) { return HttpResponse("ok"); })
      .cors(std::move(per));

  // Actual request with allowed per-route origin
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/per";
  opt.headers = {{"Origin", "https://per.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  auto originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://per.example");

  // Actual request with origin allowed by router default but not per-route -> should be denied
  opt.headers = {{"Origin", "https://app.example"}};
  raw = test::requestOrThrow(ts.port(), opt);
  parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeForbidden);

  // Preflight for per-route allowed origin
  test::RequestOptions pre;
  pre.method = "OPTIONS";
  pre.target = "/per";
  pre.headers = {{"Origin", "https://per.example"}, {"Access-Control-Request-Method", "GET"}};

  raw = test::requestOrThrow(ts.port(), pre);
  parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeNoContent);
  originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://per.example");
}

TEST(HttpCorsDetailed, PreflightWithCredentialsEmitsMirroredOriginAndCredentials) {
  CorsPolicy policy = MakePolicy();
  policy.allowCredentials(true);
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};

  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequest&) { return HttpResponse("ok"); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"}, {"Access-Control-Request-Method", "GET"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeNoContent);

  auto originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://app.example");

  auto credIt = parsed.headers.find(http::AccessControlAllowCredentials);
  ASSERT_NE(credIt, parsed.headers.end());
  EXPECT_EQ(credIt->second, "true");
}

TEST(HttpCorsDetailed, ActualRequestWithCredentialsEmitsCredentials) {
  CorsPolicy policy = MakePolicy();
  policy.allowCredentials(true);
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequest&) { return HttpResponse("ok"); });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);

  auto originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://app.example");

  auto credIt = parsed.headers.find(http::AccessControlAllowCredentials);
  ASSERT_NE(credIt, parsed.headers.end());
  EXPECT_EQ(credIt->second, "true");
}

TEST(HttpCorsDetailed, PreflightExposeHeadersAndMaxAge) {
  CorsPolicy policy = MakePolicy();
  policy.exposeHeader("X-My-Header").maxAge(std::chrono::seconds{600});
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequest&) { return HttpResponse("ok"); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"}, {"Access-Control-Request-Method", "GET"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeNoContent);

  auto exposIt = parsed.headers.find(http::AccessControlExposeHeaders);
  ASSERT_NE(exposIt, parsed.headers.end());
  EXPECT_EQ(exposIt->second, "X-My-Header");

  auto maxAgeIt = parsed.headers.find(http::AccessControlMaxAge);
  ASSERT_NE(maxAgeIt, parsed.headers.end());
  EXPECT_EQ(maxAgeIt->second, "600");
}

TEST(HttpCorsDetailed, PreflightPrivateNetworkHeader) {
  CorsPolicy policy = MakePolicy();
  policy.allowPrivateNetwork(true);
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequest&) { return HttpResponse("ok"); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"}, {"Access-Control-Request-Method", "GET"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeNoContent);

  auto pnetIt = parsed.headers.find(http::AccessControlAllowPrivateNetwork);
  ASSERT_NE(pnetIt, parsed.headers.end());
  EXPECT_EQ(pnetIt->second, "true");
}

TEST(HttpCorsDetailed, PreflightRequestedHeaderDeniedWhenNotAllowed) {
  CorsPolicy policy;
  policy.allowOrigin("https://app.example");
  policy.allowMethods(http::Method::GET | http::Method::POST);
  policy.allowRequestHeader("X-Foo");
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"},
                 {"Access-Control-Request-Method", "GET"},
                 {"Access-Control-Request-Headers", "X-Bar"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeForbidden);
  EXPECT_EQ(parsed.headers.count(http::AccessControlAllowHeaders), 0);
}

TEST(HttpCorsDetailed, PreflightEchoesRequestedHeadersWhenNoAllowedList) {
  CorsPolicy policy;
  policy.allowOrigin("https://app.example");
  policy.allowMethods(http::Method::GET | http::Method::POST);
  // Do not call allowAnyRequestHeaders or allowRequestHeader -> allowedRequestHeaders empty
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"},
                 {"Access-Control-Request-Method", "GET"},
                 {"Access-Control-Request-Headers", "  X-Trace , X-Other  "}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  // When no allowed-request-headers are configured and we did not call allowAnyRequestHeaders(),
  // a non-empty requested header list should be denied (HeadersDenied -> 403).
  EXPECT_EQ(parsed.statusCode, http::StatusCodeForbidden);
  EXPECT_EQ(parsed.headers.count(http::AccessControlAllowHeaders), 0);
}

TEST(HttpCorsDetailed, VaryIncludesOriginWhenMirroring) {
  // Case 1: no existing Vary -> should add 'Origin'
  {
    CorsPolicy policy;
    policy.allowOrigin("https://app.example").allowCredentials(true);
    RouterConfig routerCfg;
    routerCfg.withDefaultCorsPolicy(std::move(policy));

    ts.router() = Router{routerCfg};
    ts.router().setPath(http::Method::GET, "/data", [](const HttpRequest&) { return HttpResponse("ok"); });

    test::RequestOptions opt;
    opt.method = "GET";
    opt.target = "/data";
    opt.headers = {{"Origin", "https://app.example"}};

    auto raw = test::requestOrThrow(ts.port(), opt);
    auto parsed = test::parseResponseOrThrow(raw);
    EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);

    auto varyIt = parsed.headers.find(http::Vary);
    ASSERT_NE(varyIt, parsed.headers.end());
    EXPECT_TRUE(varyIt->second.contains(http::Origin));
  }

  // Case 2: existing Vary -> should append ', Origin'
  {
    CorsPolicy policy;
    policy.allowOrigin("https://app.example").allowCredentials(true);
    RouterConfig routerCfg;
    routerCfg.withDefaultCorsPolicy(std::move(policy));

    ts.router() = Router{routerCfg};
    ts.router().setPath(http::Method::GET, "/data", [](const HttpRequest&) {
      HttpResponse resp(http::StatusCodeOK);
      resp.header(http::Vary, http::AcceptEncoding);
      return resp;
    });

    test::RequestOptions opt;
    opt.method = "GET";
    opt.target = "/data";
    opt.headers = {{"Origin", "https://app.example"}};

    auto raw = test::requestOrThrow(ts.port(), opt);
    auto parsed = test::parseResponseOrThrow(raw);
    EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);

    auto varyIt = parsed.headers.find(http::Vary);
    ASSERT_NE(varyIt, parsed.headers.end());
    EXPECT_TRUE(varyIt->second.contains(http::AcceptEncoding));
    EXPECT_TRUE(varyIt->second.contains(http::Origin));
  }
}

TEST(HttpCorsDetailed, VaryNoDuplicateWhenOriginAlreadyPresent) {
  CorsPolicy policy;
  policy.allowOrigin("https://app.example").allowCredentials(true);
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequest&) {
    HttpResponse resp(http::StatusCodeOK);
    resp.addHeader(http::Vary, http::Origin);
    return resp;
  });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);

  auto varyIt = parsed.headers.find(http::Vary);
  ASSERT_NE(varyIt, parsed.headers.end());
  EXPECT_TRUE(varyIt->second.contains(http::Origin));
  std::string expectedEndOrigin = ", ";
  expectedEndOrigin += http::Origin;
  EXPECT_FALSE(varyIt->second.contains(expectedEndOrigin));
}

TEST(HttpCorsDetailed, MultipleAllowedOriginsMirrorCorrectOne) {
  CorsPolicy policy;
  policy.allowOrigin("https://one.example");
  policy.allowOrigin("https://two.example");
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequest&) { return HttpResponse("ok"); });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://two.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);

  auto originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://two.example");
}

TEST(HttpCorsDetailed, OptionsWithoutAcrMethodTreatedAsSimpleCors) {
  CorsPolicy policy;
  policy.allowOrigin("https://app.example").allowMethods(static_cast<http::MethodBmp>(http::Method::GET));
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};
  ts.router().setPath(http::Method::OPTIONS, "/data",
                      [](const HttpRequest&) { return HttpResponse(http::StatusCodeNoContent); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"}};  // no Access-Control-Request-Method header

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeNoContent);
  auto originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://app.example");
}
