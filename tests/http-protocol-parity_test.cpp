// The request pipeline scenarios run over HTTP/1.1 and over cleartext HTTP/2 (prior knowledge), through the aeronet
// HttpClient: both protocols must answer them the same way.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/client-protocol.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/test_server_fixture.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/request-task.hpp"
#endif

namespace aeronet {
namespace {

constexpr std::string_view kAllowedOrigin = "https://app.example";
constexpr std::string_view kDeniedOrigin = "https://evil.example";

HttpServerConfig MakeServerConfig() {
  HttpServerConfig cfg;
  cfg.withPollInterval(std::chrono::milliseconds{5});
  return cfg;
}

CorsPolicy MakeCorsPolicy() {
  CorsPolicy policy(CorsPolicy::Active::On);
  policy.allowOrigin(kAllowedOrigin).allowMethods(http::Method::GET | http::Method::POST);
  return policy;
}

// The response middleware tagging every response, to check that a response went through it.
void TagResponse(const HttpRequestView& /*request*/, HttpResponse& response) { response.headerAddLine("x-post", "1"); }

class ProtocolParity : public ::testing::TestWithParam<HttpVersionMode> {
 protected:
  explicit ProtocolParity(HttpServerConfig cfg = MakeServerConfig(), RouterConfig routerCfg = {})
      : ts(std::move(cfg), std::move(routerCfg)) {}

  [[nodiscard]] static HttpClient MakeClient() {
    HttpClientConfig cfg;
    cfg.withHttpVersion(GetParam());
    cfg.followRedirects = false;
    return HttpClient(std::move(cfg));
  }

  [[nodiscard]] std::string url(std::string_view target) const {
    return std::format("http://127.0.0.1:{}{}", ts.port(), target);
  }

  // Sends the request built by 'build' from a client request to 'target' and returns the response.
  template <class Build>
  [[nodiscard]] HttpResponse send(http::Method method, std::string_view target, Build&& build) {
    HttpClient client = MakeClient();
    auto request = client.makeRequest(method, url(target));
    std::forward<Build>(build)(request);
    auto result = client.request(std::move(request));
    if (!result) {
      throw std::runtime_error(std::format("request to {} failed", target));
    }
    return std::move(result).value();
  }

  [[nodiscard]] HttpResponse send(http::Method method, std::string_view target) {
    return send(method, target, [](auto&) {});
  }

  test::TestServer ts;
};

TEST_P(ProtocolParity, NotFound) {
  ts.router().addResponseMiddleware(TagResponse);
  const auto resp = send(http::Method::GET, "/missing");
  EXPECT_EQ(resp.status(), http::StatusCodeNotFound);
  EXPECT_EQ(resp.headerValueOrEmpty("x-post"), "1");
}

TEST_P(ProtocolParity, MethodNotAllowed) {
  ts.router().addResponseMiddleware(TagResponse);
  ts.router().setPath(http::Method::GET, "/get-only", [](const HttpRequestView&) { return HttpResponse("ok"); });
  const auto resp = send(http::Method::POST, "/get-only", [](auto& req) { req.body("data"); });
  EXPECT_EQ(resp.status(), http::StatusCodeMethodNotAllowed);
  EXPECT_EQ(resp.bodyInMemory(), http::ReasonMethodNotAllowed);
  EXPECT_EQ(resp.headerValueOrEmpty("x-post"), "1");
}

TEST_P(ProtocolParity, HandlerExceptionAnswers500ThroughResponseMiddleware) {
  ts.router().addResponseMiddleware(TagResponse);
  ts.router().setPath(http::Method::GET, "/throw",
                      [](const HttpRequestView&) -> HttpResponse { throw std::runtime_error("boom"); });
  const auto resp = send(http::Method::GET, "/throw");
  EXPECT_EQ(resp.status(), http::StatusCodeInternalServerError);
  EXPECT_EQ(resp.headerValueOrEmpty("x-post"), "1");
}

TEST_P(ProtocolParity, RequestMiddlewareSeesPathAndQueryParameters) {
  std::mutex mutex;
  std::string seen;
  ts.router().addRequestMiddleware([&](HttpRequestView& req) {
    std::scoped_lock lock(mutex);
    seen = std::format("{}|{}", req.pathParamValueOrEmpty("id"), req.queryParamValueOrEmpty("q"));
    return MiddlewareResult::Continue();
  });
  ts.router().setPath(http::Method::GET, "/items/{id}", [](const HttpRequestView& req) {
    return HttpResponse(std::format("{}|{}", req.pathParamValueOrEmpty("id"), req.queryParamValueOrEmpty("q")));
  });
  const auto resp = send(http::Method::GET, "/items/42?q=abc");
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.bodyInMemory(), "42|abc");
  std::scoped_lock lock(mutex);
  EXPECT_EQ(seen, "42|abc");
}

TEST_P(ProtocolParity, ShortCircuitGoesThroughResponseMiddleware) {
  std::atomic<int> handlerCalls{0};
  ts.router().addRequestMiddleware([](HttpRequestView&) {
    return MiddlewareResult::ShortCircuit(HttpResponse(http::StatusCodeUnauthorized).body("authentication required"));
  });
  ts.router().addResponseMiddleware(TagResponse);
  ts.router().setPath(http::Method::GET, "/secret", [&handlerCalls](const HttpRequestView&) {
    ++handlerCalls;
    return HttpResponse("secret");
  });
  const auto resp = send(http::Method::GET, "/secret");
  EXPECT_EQ(resp.status(), http::StatusCodeUnauthorized);
  EXPECT_EQ(resp.bodyInMemory(), "authentication required");
  EXPECT_EQ(resp.headerValueOrEmpty("x-post"), "1");
  EXPECT_EQ(handlerCalls.load(), 0);
}

TEST_P(ProtocolParity, MiddlewareMetricsCallbackFires) {
  std::atomic<int> nbPre{0};
  std::atomic<int> nbPost{0};
  ts.server.setMiddlewareMetricsCallback(
      [&](const MiddlewareMetrics& metrics) { ++(metrics.phase == MiddlewareMetrics::Phase::Pre ? nbPre : nbPost); });
  ts.router().addRequestMiddleware([](HttpRequestView&) { return MiddlewareResult::Continue(); });
  ts.router().addResponseMiddleware(TagResponse);
  ts.router()
      .setPath(http::Method::GET, "/metrics", [](const HttpRequestView&) { return HttpResponse("ok"); })
      .before([](HttpRequestView&) { return MiddlewareResult::Continue(); })
      .after(TagResponse);
  const auto resp = send(http::Method::GET, "/metrics");
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  // The callback runs before the response is sent.
  EXPECT_EQ(nbPre.load(), 2);
  EXPECT_EQ(nbPost.load(), 2);
}

TEST_P(ProtocolParity, CorsDeniedOriginDoesNotRunHandler) {
  std::atomic<int> handlerCalls{0};
  ts.router().addResponseMiddleware(TagResponse);
  ts.router()
      .setPath(http::Method::POST, "/cors",
               [&handlerCalls](const HttpRequestView&) {
                 ++handlerCalls;
                 return HttpResponse("side effect");
               })
      .cors(MakeCorsPolicy());
  const auto denied = send(http::Method::POST, "/cors", [](auto& req) {
    req.headerAddLine(http::Origin, kDeniedOrigin);
    req.body("data");
  });
  EXPECT_EQ(denied.status(), http::StatusCodeForbidden);
  EXPECT_EQ(denied.headerValueOrEmpty("x-post"), "1");
  EXPECT_EQ(handlerCalls.load(), 0);

  const auto allowed = send(http::Method::POST, "/cors", [](auto& req) {
    req.headerAddLine(http::Origin, kAllowedOrigin);
    req.body("data");
  });
  EXPECT_EQ(allowed.status(), http::StatusCodeOK);
  EXPECT_EQ(allowed.headerValueOrEmpty(http::AccessControlAllowOrigin), kAllowedOrigin);
  EXPECT_EQ(handlerCalls.load(), 1);
}

TEST_P(ProtocolParity, CorsDeniedOriginDoesNotRunStreamingHandler) {
  std::atomic<int> handlerCalls{0};
  ts.router()
      .setPath(http::Method::POST, "/cors-stream",
               [&handlerCalls](const HttpRequestView&, HttpResponseWriter& writer) {
                 ++handlerCalls;
                 writer.writeBody("side effect");
                 writer.end();
               })
      .cors(MakeCorsPolicy());
  const auto resp = send(http::Method::POST, "/cors-stream", [](auto& req) {
    req.headerAddLine(http::Origin, kDeniedOrigin);
    req.body("data");
  });
  EXPECT_EQ(resp.status(), http::StatusCodeForbidden);
  EXPECT_EQ(handlerCalls.load(), 0);
}

TEST_P(ProtocolParity, StreamingResponseGoesThroughResponseMiddlewareAndCors) {
  ts.router().addResponseMiddleware(TagResponse);
  ts.router()
      .setPath(http::Method::GET, "/stream",
               [](const HttpRequestView&, HttpResponseWriter& writer) {
                 writer.writeBody("alpha-");
                 writer.writeBody("beta");
                 writer.end();
               })
      .cors(MakeCorsPolicy());
  const auto resp =
      send(http::Method::GET, "/stream", [](auto& req) { req.headerAddLine(http::Origin, kAllowedOrigin); });
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.bodyInMemory(), "alpha-beta");
  EXPECT_EQ(resp.headerValueOrEmpty("x-post"), "1");
  EXPECT_EQ(resp.headerValueOrEmpty(http::AccessControlAllowOrigin), kAllowedOrigin);
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
// 1xx, 204 and 304 responses carry no content, whatever the handler set: no body, no Content-Length, no Content-Type.
TEST_P(ProtocolParity, StatusesWithoutContentDropTheBody) {
  for (http::StatusCode status : {http::StatusCodeNoContent, http::StatusCodeNotModified}) {
    ts.router().setPath(http::Method::GET, std::format("/buffered{}", status), [status](const HttpRequestView&) {
      HttpResponse resp(status, "BODY-OF-RESPONSE");
      resp.trailerAddLine("x-trailer", "t").headerAddLine("x-custom", "kept");
      return resp;
    });
    ts.router().setPath(http::Method::GET, std::format("/streamed{}", status),
                        [status](const HttpRequestView&, HttpResponseWriter& writer) {
                          writer.status(status);
                          writer.headerAddLine("x-custom", "kept");
                          writer.writeBody("STREAMED-BODY-OF-RESPONSE");
                          writer.trailerAddLine("x-trailer", "t");
                          writer.end();
                        });
    for (std::string_view kind : {"/buffered", "/streamed"}) {
      SCOPED_TRACE(std::format("{}{}", kind, status));
      const auto resp = send(http::Method::GET, std::format("{}{}", kind, status));
      EXPECT_EQ(resp.status(), status);
      EXPECT_TRUE(resp.bodyInMemory().empty());
      EXPECT_EQ(resp.trailersSize(), 0U);
      EXPECT_EQ(resp.headerValueOrEmpty("x-custom"), "kept");
      EXPECT_FALSE(resp.headerValue(http::ContentLength).has_value());
      EXPECT_FALSE(resp.headerValue(http::TransferEncoding).has_value());
      EXPECT_FALSE(resp.headerValue(http::ContentType).has_value());
    }
  }
}

TEST_P(ProtocolParity, CorsDeniedOriginDoesNotRunAsyncHandler) {
  std::atomic<int> handlerCalls{0};
  ts.router()
      .setPath(http::Method::POST, "/cors-async",
               [&handlerCalls](HttpRequestView&) -> RequestTask<HttpResponse> {
                 ++handlerCalls;
                 co_return HttpResponse("side effect");
               })
      .cors(MakeCorsPolicy());
  const auto resp = send(http::Method::POST, "/cors-async", [](auto& req) {
    req.headerAddLine(http::Origin, kDeniedOrigin);
    req.body("data");
  });
  EXPECT_EQ(resp.status(), http::StatusCodeForbidden);
  EXPECT_EQ(handlerCalls.load(), 0);
}

TEST_P(ProtocolParity, AsyncHandlerExceptionAnswers500ThroughResponseMiddleware) {
  ts.router().addResponseMiddleware(TagResponse);
  // A coroutine throwing from its body.
  ts.router().setPath(http::Method::GET, "/async-throw", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
    if (!req.path().empty()) {
      throw std::runtime_error("boom");
    }
    co_return HttpResponse("unreachable");
  });
  // A function returning a RequestTask without being a coroutine, throwing when called.
  ts.router().setPath(http::Method::GET, "/async-throw-on-call",
                      [](HttpRequestView&) -> RequestTask<HttpResponse> { throw std::runtime_error("boom"); });
  for (const std::string_view target : {"/async-throw", "/async-throw-on-call"}) {
    const auto resp = send(http::Method::GET, target);
    EXPECT_EQ(resp.status(), http::StatusCodeInternalServerError) << target;
    EXPECT_EQ(resp.headerValueOrEmpty("x-post"), "1") << target;
  }
  // The server still serves requests.
  EXPECT_EQ(send(http::Method::GET, "/missing").status(), http::StatusCodeNotFound);
}
#endif

TEST_P(ProtocolParity, CorsPreflight) {
  ts.router()
      .setPath(http::Method::POST, "/cors", [](const HttpRequestView&) { return HttpResponse("ok"); })
      .cors(MakeCorsPolicy());
  const auto resp = send(http::Method::OPTIONS, "/cors", [](auto& req) {
    req.headerAddLine(http::Origin, kAllowedOrigin);
    req.headerAddLine(http::AccessControlRequestMethod, "POST");
  });
  EXPECT_EQ(resp.status(), http::StatusCodeNoContent);
  EXPECT_EQ(resp.headerValueOrEmpty(http::AccessControlAllowOrigin), kAllowedOrigin);
}

class ProtocolParityTrailingSlashRedirect : public ProtocolParity {
 protected:
  ProtocolParityTrailingSlashRedirect()
      : ProtocolParity(MakeServerConfig(),
                       RouterConfig{}.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect)) {}
};

TEST_P(ProtocolParityTrailingSlashRedirect, RedirectsToCanonicalPath) {
  ts.router().addResponseMiddleware(TagResponse);
  ts.router().setPath(http::Method::GET, "/foo", [](const HttpRequestView&) { return HttpResponse("foo"); });
  ts.router().setPath(http::Method::GET, "/bar/", [](const HttpRequestView&) { return HttpResponse("bar"); });

  const auto removeSlash = send(http::Method::GET, "/foo/");
  EXPECT_EQ(removeSlash.status(), http::StatusCodeMovedPermanently);
  EXPECT_EQ(removeSlash.headerValueOrEmpty(http::Location), "/foo");
  EXPECT_EQ(removeSlash.headerValueOrEmpty("x-post"), "1");

  const auto addSlash = send(http::Method::GET, "/bar");
  EXPECT_EQ(addSlash.status(), http::StatusCodeMovedPermanently);
  EXPECT_EQ(addSlash.headerValueOrEmpty(http::Location), "/bar/");

  EXPECT_EQ(send(http::Method::GET, "/foo").bodyInMemory(), "foo");
}

constexpr uint16_t kHttpsPort = 8443;

class ProtocolParityHttpsRedirect : public ProtocolParity {
 protected:
  ProtocolParityHttpsRedirect() : ProtocolParity(MakeServerConfig().withHttpsRedirect(kHttpsPort)) {}
};

TEST_P(ProtocolParityHttpsRedirect, PlaintextRequestsAreRedirected) {
  std::atomic<int> handlerCalls{0};
  ts.router().setPath(http::Method::GET | http::Method::POST, "/page", [&handlerCalls](const HttpRequestView&) {
    ++handlerCalls;
    return HttpResponse("served in cleartext");
  });

  const auto resp = send(http::Method::GET, "/page?a=1&b=two%20words");
  EXPECT_EQ(resp.status(), http::StatusCodeMovedPermanently);
  EXPECT_EQ(resp.headerValueOrEmpty(http::Location),
            std::format("https://127.0.0.1:{}/page?a=1&b=two%20words", kHttpsPort));

  // The body of the request is not read.
  const auto post = send(http::Method::POST, "/page", [](auto& req) { req.body(std::string(256UL * 1024UL, 'b')); });
  EXPECT_EQ(post.status(), http::StatusCodeMovedPermanently);
  EXPECT_EQ(handlerCalls.load(), 0);
}

std::string ProtocolName(const ::testing::TestParamInfo<HttpVersionMode>& info) {
  return {info.param == HttpVersionMode::Http2 ? "Http2" : "Http1"};
}

INSTANTIATE_TEST_SUITE_P(Protocols, ProtocolParity, ::testing::Values(HttpVersionMode::Http1_1, HttpVersionMode::Http2),
                         ProtocolName);
INSTANTIATE_TEST_SUITE_P(Protocols, ProtocolParityTrailingSlashRedirect,
                         ::testing::Values(HttpVersionMode::Http1_1, HttpVersionMode::Http2), ProtocolName);
INSTANTIATE_TEST_SUITE_P(Protocols, ProtocolParityHttpsRedirect,
                         ::testing::Values(HttpVersionMode::Http1_1, HttpVersionMode::Http2), ProtocolName);

}  // namespace
}  // namespace aeronet
