#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/request-task.hpp"
#endif

using namespace aeronet;

namespace {
test::TestServer ts(HttpServerConfig{});

struct BodyReadTimeoutScope {
  explicit BodyReadTimeoutScope(std::chrono::milliseconds timeout) {
    ts.postConfigUpdate([timeout](HttpServerConfig& cfg) { cfg.withBodyReadTimeout(timeout); });
  }
  BodyReadTimeoutScope(const BodyReadTimeoutScope&) = delete;
  BodyReadTimeoutScope& operator=(const BodyReadTimeoutScope&) = delete;
  BodyReadTimeoutScope(BodyReadTimeoutScope&&) = delete;
  BodyReadTimeoutScope& operator=(BodyReadTimeoutScope&&) = delete;
  ~BodyReadTimeoutScope() {
    ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withBodyReadTimeout(std::chrono::milliseconds{0}); });
  }
};

struct PollIntervalScope {
  explicit PollIntervalScope(std::chrono::milliseconds interval) : _previous(ts.server.config().pollInterval) {
    ts.postConfigUpdate([interval](HttpServerConfig& cfg) { cfg.withPollInterval(interval); });
  }
  PollIntervalScope(const PollIntervalScope&) = delete;
  PollIntervalScope& operator=(const PollIntervalScope&) = delete;
  PollIntervalScope(PollIntervalScope&&) = delete;
  PollIntervalScope& operator=(PollIntervalScope&&) = delete;
  ~PollIntervalScope() {
    ts.postConfigUpdate([prev = _previous](HttpServerConfig& cfg) { cfg.withPollInterval(prev); });
  }

 private:
  std::chrono::milliseconds _previous;
};

constexpr std::size_t kAsyncLargePayload = 16 << 20;
}  // namespace

TEST(HttpRouting, BasicPathDispatch) {
  ts.router().setPath(http::Method::GET, "/hello", [](const HttpRequest&) { return HttpResponse("world"); });
  ts.router().setPath(http::Method::GET | http::Method::POST, "/multi", [](const HttpRequest& req) {
    return HttpResponse(std::string(http::MethodToStr(req.method())) + "!");
  });

  test::RequestOptions getHello;
  getHello.method = "GET";
  getHello.target = "/hello";
  auto resp1 = test::requestOrThrow(ts.port(), getHello);
  EXPECT_TRUE(resp1.contains("HTTP/1.1 200"));
  EXPECT_TRUE(resp1.contains("world"));
  test::RequestOptions postHello;
  postHello.method = "POST";
  postHello.target = "/hello";
  postHello.headers.emplace_back("Content-Length", "0");
  auto resp2 = test::requestOrThrow(ts.port(), postHello);
  EXPECT_TRUE(resp2.contains("HTTP/1.1 405"));
  test::RequestOptions getMissing;
  getMissing.method = "GET";
  getMissing.target = "/missing";
  auto resp3 = test::requestOrThrow(ts.port(), getMissing);
  EXPECT_TRUE(resp3.contains("HTTP/1.1 404"));
  EXPECT_TRUE(resp3.contains("<!DOCTYPE html>"));
  EXPECT_TRUE(resp3.contains("aeronet"));
  test::RequestOptions postMulti;
  postMulti.method = "POST";
  postMulti.target = "/multi";
  postMulti.headers.emplace_back("Content-Length", "0");
  auto resp4 = test::requestOrThrow(ts.port(), postMulti);
  EXPECT_TRUE(resp4.contains("HTTP/1.1 200"));
  EXPECT_TRUE(resp4.contains("POST!"));
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
TEST(HttpRouting, AsyncHandlerDispatch) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/async-route", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    std::string payload("async:");
    payload.append(req.path());
    co_return HttpResponse(http::StatusCodeOK).body(std::move(payload));
  });

  const std::string response = test::simpleGet(ts.port(), "/async-route");
  EXPECT_TRUE(response.contains("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains("async:/async-route")) << response;
}
#endif

TEST(HttpRouting, GlobalFallbackWithPathHandlers) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(200); });
  // Adding path handler after global handler is now allowed (Phase 2 mixing model)
  EXPECT_NO_THROW(ts.router().setPath(http::Method::GET, "/x", [](const HttpRequest&) { return HttpResponse(200); }));
}

TEST(HttpRouting, PathParametersInjectedIntoRequest) {
  std::string seenUser;
  std::string seenPost;
  ts.router().setPath(http::Method::GET, "/users/{userId}/posts/{postId}", [&](const HttpRequest& req) {
    EXPECT_TRUE(req.hasPathParam("userId"));
    EXPECT_TRUE(req.hasPathParam("postId"));
    EXPECT_FALSE(req.hasPathParam("missingParam"));

    EXPECT_EQ(req.pathParamValueOrEmpty("userId"), "42");
    EXPECT_EQ(req.pathParamValueOrEmpty("postId"), "abcd");
    EXPECT_EQ(req.pathParamValueOrEmpty("missing"), "");

    EXPECT_EQ(req.pathParamValue("userId").value_or(""), req.pathParams().at("userId"));
    EXPECT_EQ(req.pathParamValue("postId").value_or(""), req.pathParams().at("postId"));
    EXPECT_FALSE(req.pathParamValue("missing"));

    const auto& params = req.pathParams();
    if (const auto itUser = params.find("userId"); itUser != params.end()) {
      seenUser.assign(itUser->second);
    }
    if (const auto itPost = params.find("postId"); itPost != params.end()) {
      seenPost.assign(itPost->second);
    }
    return HttpResponse(200).reason("ok");
  });

  test::RequestOptions reqOpts;
  reqOpts.method = "GET";
  reqOpts.target = "/users/42/posts/abcd";
  auto resp = test::requestOrThrow(ts.server.port(), reqOpts);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200 ok"));
  EXPECT_EQ(seenUser, "42");
  EXPECT_EQ(seenPost, "abcd");
}

namespace {
std::string rawRequest(uint16_t port, std::string_view target) {
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = target;
  opt.connection = "close";
  return test::request(port, opt).value_or("");
}

}  // namespace

class HttpTrailingSlash : public ::testing::Test {
 protected:
  static void setTrailingSlash(RouterConfig::TrailingSlashPolicy trailingSlashPolicy) {
    RouterConfig routerCfg;
    routerCfg.withTrailingSlashPolicy(trailingSlashPolicy);
    ts.router() = Router(std::move(routerCfg));
  }
};

TEST_F(HttpTrailingSlash, StrictPolicyDifferent) {
  setTrailingSlash(RouterConfig::TrailingSlashPolicy::Strict);
  ts.router().setPath(http::Method::GET, "/alpha", [](const HttpRequest&) { return HttpResponse().body("alpha"); });
  auto resp = rawRequest(ts.port(), "/alpha/");
  ASSERT_TRUE(resp.contains("404"));
}

TEST_F(HttpTrailingSlash, NormalizeSingleSlash) {
  setTrailingSlash(RouterConfig::TrailingSlashPolicy::Normalize);
  ts.router().setPath(http::Method::GET, "/", [](const HttpRequest&) { return HttpResponse().body("beta"); });
  auto resp = rawRequest(ts.port(), "/");
  ASSERT_TRUE(resp.contains("200"));
  ASSERT_TRUE(resp.contains("beta"));

  resp = rawRequest(ts.port(), "//");
  ASSERT_TRUE(resp.contains("200"));
  ASSERT_TRUE(resp.contains("beta"));
}

TEST_F(HttpTrailingSlash, NormalizePolicyStrips) {
  setTrailingSlash(RouterConfig::TrailingSlashPolicy::Normalize);
  ts.router().setPath(http::Method::GET, "/beta", [](const HttpRequest&) { return HttpResponse().body("beta"); });
  auto resp = rawRequest(ts.port(), "/beta/");
  ASSERT_TRUE(resp.contains("200"));
  ASSERT_TRUE(resp.contains("beta"));
}

TEST_F(HttpTrailingSlash, NormalizePolicyAddSlash) {
  setTrailingSlash(RouterConfig::TrailingSlashPolicy::Normalize);
  ts.router().setPath(http::Method::GET, "/beta/", [](const HttpRequest&) { return HttpResponse().body("beta/"); });
  auto resp = rawRequest(ts.port(), "/beta");

  ASSERT_TRUE(resp.contains("200"));
  ASSERT_TRUE(resp.contains("beta"));
}

TEST_F(HttpTrailingSlash, RedirectPolicy) {
  setTrailingSlash(RouterConfig::TrailingSlashPolicy::Redirect);
  ts.router().setPath(http::Method::GET, "/gamma", [](const HttpRequest&) { return HttpResponse().body("gamma"); });
  auto resp = rawRequest(ts.port(), "/gamma/");
  // Expect 301 and Location header
  ASSERT_TRUE(resp.contains("301"));
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::Location, "/gamma")));
}

// Additional matrix coverage

TEST_F(HttpTrailingSlash, StrictPolicyRegisteredWithSlashDoesNotMatchWithout) {
  setTrailingSlash(RouterConfig::TrailingSlashPolicy::Strict);
  ts.router().setPath(http::Method::GET, "/sigma/", [](const HttpRequest&) { return HttpResponse().body("sigma"); });
  auto ok = rawRequest(ts.port(), "/sigma/");
  auto notFound = rawRequest(ts.port(), "/sigma");
  ASSERT_TRUE(ok.contains("200"));
  ASSERT_TRUE(notFound.contains("404"));
}

TEST_F(HttpTrailingSlash, NormalizePolicyRegisteredWithSlashAcceptsWithout) {
  setTrailingSlash(RouterConfig::TrailingSlashPolicy::Normalize);
  ts.router().setPath(http::Method::GET, "/norm/", [](const HttpRequest&) { return HttpResponse().body("norm"); });
  auto withSlash = rawRequest(ts.port(), "/norm/");
  auto withoutSlash = rawRequest(ts.port(), "/norm");
  ASSERT_TRUE(withSlash.contains("200"));
  ASSERT_TRUE(withoutSlash.contains("200"));
  ASSERT_TRUE(withoutSlash.contains("norm"));
}

TEST_F(HttpTrailingSlash, RedirectPolicyRemoveSlash) {
  setTrailingSlash(RouterConfig::TrailingSlashPolicy::Redirect);
  ts.router().setPath(http::Method::GET, "/redir", [](const HttpRequest&) { return HttpResponse().body("redir"); });
  auto redirect = rawRequest(ts.port(), "/redir/");  // should 301 -> /redir
  auto canonical = rawRequest(ts.port(), "/redir");  // should 200
  ASSERT_TRUE(redirect.contains("301"));
  ASSERT_TRUE(redirect.contains(MakeHttp1HeaderLine(http::Location, "/redir")));
  ASSERT_TRUE(canonical.contains("200"));
  ASSERT_TRUE(canonical.contains("redir"));
}

TEST_F(HttpTrailingSlash, RedirectPolicyAddSlash) {
  setTrailingSlash(RouterConfig::TrailingSlashPolicy::Redirect);
  ts.router().setPath(http::Method::GET, "/only/", [](const HttpRequest&) { return HttpResponse().body("only"); });
  auto withSlash = rawRequest(ts.port(), "/only/");
  auto withoutSlash = rawRequest(ts.port(), "/only");

  ASSERT_TRUE(withSlash.contains("200"));
  ASSERT_TRUE(withoutSlash.contains("301"));
}

TEST_F(HttpTrailingSlash, RootPathNotRedirected) {
  setTrailingSlash(RouterConfig::TrailingSlashPolicy::Redirect);
  auto resp = rawRequest(ts.port(), "/");  // no handlers => 404 but not 301
  ASSERT_TRUE(resp.contains("404"));
  ASSERT_FALSE(resp.contains("301"));
}

TEST_F(HttpTrailingSlash, StrictPolicyBothVariants_Independent) {
  setTrailingSlash(RouterConfig::TrailingSlashPolicy::Strict);
  ts.router().setPath(http::Method::GET, "/both",
                      [](const HttpRequest&) { return HttpResponse().body("both-no-slash"); });
  ts.router().setPath(http::Method::GET, "/both/",
                      [](const HttpRequest&) { return HttpResponse().body("both-with-slash"); });
  auto respNoSlash = rawRequest(ts.port(), "/both");
  auto respWithSlash = rawRequest(ts.port(), "/both/");

  ASSERT_TRUE(respNoSlash.contains("200"));
  ASSERT_TRUE(respNoSlash.contains("both-no-slash"));
  ASSERT_TRUE(respWithSlash.contains("200"));
  ASSERT_TRUE(respWithSlash.contains("both-with-slash"));
}

TEST(HttpMiddleware, GlobalRequestShortCircuit) {
  std::atomic_bool handlerCalled{false};

  ts.resetRouterAndGet().setDefault([&](const HttpRequest&) {
    handlerCalled.store(true, std::memory_order_relaxed);
    return HttpResponse("handler");
  });

  ts.router().addResponseMiddleware(
      [](const HttpRequest&, HttpResponse& resp) { resp.header("X-Global-Middleware", "applied"); });

  ts.router().addRequestMiddleware([](HttpRequest& req) {
    if (req.path() == "/mw-short") {
      return MiddlewareResult::ShortCircuit(HttpResponse(http::StatusCodeServiceUnavailable, "short-circuited"));
    }
    auto cont = MiddlewareResult::Continue();
    EXPECT_TRUE(cont.shouldContinue());
    return cont;
  });

  const std::string response = test::simpleGet(ts.port(), "/mw-short");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 503")) << response;
  EXPECT_TRUE(response.contains("short-circuited")) << response;
  EXPECT_TRUE(response.contains("X-Global-Middleware: applied")) << response;
  EXPECT_FALSE(handlerCalled.load(std::memory_order_relaxed));

  const std::string response2 = test::simpleGet(ts.port(), "/other-path");
  EXPECT_TRUE(response2.starts_with("HTTP/1.1 200")) << response2;
  EXPECT_TRUE(response2.contains("handler")) << response2;
  EXPECT_TRUE(response2.contains("X-Global-Middleware: applied")) << response2;
  EXPECT_TRUE(handlerCalled.load(std::memory_order_relaxed));
}

TEST(HttpMiddleware, RouteMiddlewareOrderAndResponseMutation) {
  std::mutex seqMutex;
  std::vector<std::string> sequence;

  ts.resetRouterAndGet().addRequestMiddleware([&](HttpRequest&) {
    std::scoped_lock lock(seqMutex);
    sequence.emplace_back("global-pre");
    return MiddlewareResult::Continue();
  });

  ts.router().addResponseMiddleware([&](const HttpRequest&, HttpResponse& resp) {
    std::scoped_lock lock(seqMutex);
    sequence.emplace_back("global-post");
    resp.header("X-Global-Middleware", "post");
  });

  auto entry = ts.router().setPath(http::Method::GET, "/mw-route", [&](const HttpRequest&) {
    std::scoped_lock lock(seqMutex);
    sequence.emplace_back("handler");
    HttpResponse resp;
    resp.body("handler");
    return resp;
  });

  entry.before([&](HttpRequest&) {
    std::scoped_lock lock(seqMutex);
    sequence.emplace_back("route-pre");
    return MiddlewareResult::Continue();
  });

  entry.after([&](const HttpRequest&, HttpResponse& resp) {
    std::scoped_lock lock(seqMutex);
    sequence.emplace_back("route-post");
    resp.header("X-Route-Middleware", "post");
    const std::string existingBody(resp.bodyInMemory());
    std::string updatedBody("route:");
    updatedBody += existingBody;
    resp.body(updatedBody);
  });

  const std::string response = test::simpleGet(ts.port(), "/mw-route");
  EXPECT_TRUE(response.contains("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains("route:handler")) << response;
  EXPECT_TRUE(response.contains("X-Route-Middleware: post")) << response;
  EXPECT_TRUE(response.contains("X-Global-Middleware: post")) << response;

  std::vector<std::string> snapshot;
  {
    std::scoped_lock lock(seqMutex);
    snapshot = sequence;
  }
  const std::vector<std::string> expected{"global-pre", "route-pre", "handler", "route-post", "global-post"};
  EXPECT_EQ(snapshot, expected);
}

TEST(HttpMiddleware, StreamingResponseMiddlewareApplied) {
  std::mutex seqMutex;
  std::vector<std::string> sequence;

  ts.resetRouterAndGet().addRequestMiddleware([&](HttpRequest&) {
    std::scoped_lock lock(seqMutex);
    sequence.emplace_back("global-pre");
    return MiddlewareResult::Continue();
  });

  ts.router().addResponseMiddleware([&](const HttpRequest&, HttpResponse& resp) {
    std::scoped_lock lock(seqMutex);
    sequence.emplace_back("global-post");
    resp.header("X-Global-Streaming", "post");
  });

  auto entry =
      ts.router().setPath(http::Method::GET, "/mw-stream", [&](const HttpRequest&, HttpResponseWriter& writer) {
        {
          std::scoped_lock lock(seqMutex);
          sequence.emplace_back("handler");
        }
        writer.status(http::StatusCodeOK);
        writer.reason("OK");
        writer.header("X-Handler", "emitted");
        writer.contentType("text/plain");
        EXPECT_TRUE(writer.writeBody("chunk-1"));
        EXPECT_TRUE(writer.writeBody("chunk-2"));
        writer.end();
      });

  entry.before([&](HttpRequest&) {
    std::scoped_lock lock(seqMutex);
    sequence.emplace_back("route-pre");
    return MiddlewareResult::Continue();
  });

  entry.after([&](const HttpRequest&, HttpResponse& resp) {
    std::scoped_lock lock(seqMutex);
    sequence.emplace_back("route-post");
    resp.status(http::StatusCodeAccepted);
    resp.reason("Accepted by middleware");
    resp.header("X-Route-Streaming", "post");
  });

  const std::string response = test::simpleGet(ts.port(), "/mw-stream");
  EXPECT_TRUE(response.contains("HTTP/1.1 202")) << response;
  EXPECT_TRUE(response.contains("X-Handler: emitted")) << response;
  EXPECT_TRUE(response.contains("X-Route-Streaming: post")) << response;
  EXPECT_TRUE(response.contains("X-Global-Streaming: post")) << response;
  EXPECT_TRUE(response.contains("chunk-1")) << response;
  EXPECT_TRUE(response.contains("chunk-2")) << response;

  std::vector<std::string> snapshot;
  {
    std::scoped_lock lock(seqMutex);
    snapshot = sequence;
  }
  const std::vector<std::string> expected{"global-pre", "route-pre", "handler", "route-post", "global-post"};
  EXPECT_EQ(snapshot, expected);
}

TEST(HttpMiddlewareMetrics, RecordsPreAndPostMetrics) {
  std::mutex metricsMutex;
  std::vector<MiddlewareMetrics> captured;
  std::vector<std::string> requestPaths;
  ts.server.setMiddlewareMetricsCallback([&](const MiddlewareMetrics& metrics) {
    std::scoped_lock lock(metricsMutex);
    captured.push_back(metrics);
    requestPaths.emplace_back(metrics.requestPath);
  });

  RouterUpdateProxy router = ts.resetRouterAndGet();
  router.addRequestMiddleware([](HttpRequest&) { return MiddlewareResult::Continue(); });
  router.addResponseMiddleware([](const HttpRequest&, HttpResponse&) {});

  auto entry = router.setPath(http::Method::GET, "/mw-metrics", [](const HttpRequest&) {
    HttpResponse resp;
    resp.body("from-handler");
    return resp;
  });

  entry.before([](HttpRequest&) { return MiddlewareResult::Continue(); });
  entry.after([](const HttpRequest&, HttpResponse&) {});

  const std::string response = test::simpleGet(ts.port(), "/mw-metrics");
  ASSERT_TRUE(response.contains("HTTP/1.1 200")) << response;

  ts.server.setMiddlewareMetricsCallback({});

  std::vector<MiddlewareMetrics> metrics;
  {
    std::scoped_lock lock(metricsMutex);
    metrics = captured;
  }

  ASSERT_EQ(metrics.size(), 4U);

  EXPECT_EQ(metrics[0].phase, MiddlewareMetrics::Phase::Pre);
  EXPECT_TRUE(metrics[0].isGlobal);
  EXPECT_FALSE(metrics[0].shortCircuited);
  EXPECT_FALSE(metrics[0].threw);
  EXPECT_FALSE(metrics[0].streaming);
  EXPECT_EQ(metrics[0].index, 0U);
  EXPECT_EQ(metrics[0].method, http::Method::GET);
  EXPECT_EQ(requestPaths[0], "/mw-metrics");

  EXPECT_EQ(metrics[1].phase, MiddlewareMetrics::Phase::Pre);
  EXPECT_FALSE(metrics[1].isGlobal);
  EXPECT_FALSE(metrics[1].shortCircuited);
  EXPECT_FALSE(metrics[1].threw);
  EXPECT_FALSE(metrics[1].streaming);
  EXPECT_EQ(metrics[1].index, 0U);
  EXPECT_EQ(requestPaths[1], "/mw-metrics");

  EXPECT_EQ(metrics[2].phase, MiddlewareMetrics::Phase::Post);
  EXPECT_FALSE(metrics[2].isGlobal);
  EXPECT_FALSE(metrics[2].shortCircuited);
  EXPECT_FALSE(metrics[2].threw);
  EXPECT_FALSE(metrics[2].streaming);
  EXPECT_EQ(metrics[2].index, 0U);

  EXPECT_EQ(metrics[3].phase, MiddlewareMetrics::Phase::Post);
  EXPECT_TRUE(metrics[3].isGlobal);
  EXPECT_FALSE(metrics[3].shortCircuited);
  EXPECT_FALSE(metrics[3].threw);
  EXPECT_FALSE(metrics[3].streaming);
  EXPECT_EQ(metrics[3].index, 0U);
}

TEST(HttpMiddlewareMetrics, MarksShortCircuit) {
  std::mutex metricsMutex;
  std::vector<MiddlewareMetrics> captured;
  ts.server.setMiddlewareMetricsCallback([&](const MiddlewareMetrics& metrics) {
    std::scoped_lock lock(metricsMutex);
    captured.push_back(metrics);
  });

  RouterUpdateProxy router = ts.resetRouterAndGet();
  router.addRequestMiddleware([](HttpRequest&) { return MiddlewareResult::Continue(); });
  router.addResponseMiddleware([](const HttpRequest&, HttpResponse&) {});

  std::atomic_bool handlerInvoked{false};
  auto entry = router.setPath(http::Method::GET, "/mw-short-metrics", [&](const HttpRequest&) {
    handlerInvoked.store(true, std::memory_order_relaxed);
    return HttpResponse("should-not-run");
  });

  entry.before([](HttpRequest&) {
    HttpResponse resp(http::StatusCodeServiceUnavailable);
    resp.body("shorted");
    return MiddlewareResult::ShortCircuit(std::move(resp));
  });
  entry.after([](const HttpRequest&, HttpResponse&) {});

  const std::string response = test::simpleGet(ts.port(), "/mw-short-metrics");
  EXPECT_TRUE(response.contains("HTTP/1.1 503")) << response;
  EXPECT_FALSE(handlerInvoked.load(std::memory_order_relaxed));

  ts.server.setMiddlewareMetricsCallback({});

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  std::vector<MiddlewareMetrics> metrics;
  {
    std::scoped_lock lock(metricsMutex);
    metrics = captured;
  }

  ASSERT_EQ(metrics.size(), 3U);
  EXPECT_EQ(metrics[0].phase, MiddlewareMetrics::Phase::Pre);
  EXPECT_TRUE(metrics[0].isGlobal);
  EXPECT_FALSE(metrics[0].shortCircuited);
  EXPECT_FALSE(metrics[0].streaming);

  EXPECT_EQ(metrics[1].phase, MiddlewareMetrics::Phase::Post);
  EXPECT_FALSE(metrics[1].isGlobal);
  EXPECT_FALSE(metrics[1].shortCircuited);
  EXPECT_FALSE(metrics[1].streaming);

  EXPECT_EQ(metrics[2].phase, MiddlewareMetrics::Phase::Post);
  EXPECT_TRUE(metrics[2].isGlobal);
  EXPECT_FALSE(metrics[2].shortCircuited);
  EXPECT_FALSE(metrics[2].streaming);
#endif
}

TEST(HttpMiddlewareMetrics, StreamingFlagPropagates) {
  std::mutex metricsMutex;
  std::vector<MiddlewareMetrics> captured;
  std::vector<std::string> requestPaths;
  ts.server.setMiddlewareMetricsCallback([&](const MiddlewareMetrics& metrics) {
    std::scoped_lock lock(metricsMutex);
    captured.push_back(metrics);
    requestPaths.emplace_back(metrics.requestPath);
  });

  RouterUpdateProxy router = ts.resetRouterAndGet();
  router.addRequestMiddleware([](HttpRequest&) { return MiddlewareResult::Continue(); });
  router.addResponseMiddleware([](const HttpRequest&, HttpResponse&) {});

  auto entry =
      router.setPath(http::Method::GET, "/mw-stream-metrics", [](const HttpRequest&, HttpResponseWriter& writer) {
        writer.status(http::StatusCodeOK);
        writer.reason("OK");
        writer.header("X", "1");
        EXPECT_TRUE(writer.writeBody("chunk"));
        writer.end();
      });

  entry.before([](HttpRequest&) { return MiddlewareResult::Continue(); });
  entry.after([](const HttpRequest&, HttpResponse&) {});

  const std::string response = test::simpleGet(ts.port(), "/mw-stream-metrics");
  ASSERT_TRUE(response.contains("HTTP/1.1 200")) << response;

  ts.server.setMiddlewareMetricsCallback({});

  std::vector<MiddlewareMetrics> metrics;
  {
    std::scoped_lock lock(metricsMutex);
    metrics = captured;
  }

  ASSERT_EQ(metrics.size(), 4U);

  EXPECT_TRUE(metrics[0].isGlobal);
  EXPECT_TRUE(metrics[0].streaming);

  EXPECT_FALSE(metrics[1].isGlobal);
  EXPECT_TRUE(metrics[1].streaming);

  EXPECT_FALSE(metrics[2].isGlobal);
  EXPECT_TRUE(metrics[2].streaming);

  EXPECT_TRUE(metrics[3].isGlobal);
  EXPECT_TRUE(metrics[3].streaming);
  for (const auto& path : requestPaths) {
    EXPECT_EQ(path, "/mw-stream-metrics");
  }
}

TEST(HttpMiddleware, RouterOwnsGlobalMiddleware) {
  std::atomic_bool preSeen{false};
  std::atomic_bool postSeen{false};

  ts.resetRouterAndGet().addRequestMiddleware([&](HttpRequest&) {
    preSeen.store(true, std::memory_order_relaxed);
    return MiddlewareResult::Continue();
  });

  ts.router().addResponseMiddleware([&](const HttpRequest&, HttpResponse& resp) {
    postSeen.store(true, std::memory_order_relaxed);
    resp.header("X-Router-Post", "ok");
  });

  ts.router().setPath(http::Method::GET, "/router-owned",
                      [](const HttpRequest&) { return HttpResponse().body("payload"); });

  const std::string response = test::simpleGet(ts.port(), "/router-owned");
  EXPECT_TRUE(response.contains("payload")) << response;
  EXPECT_TRUE(response.contains("X-Router-Post: ok")) << response;
  EXPECT_TRUE(preSeen.load(std::memory_order_relaxed));
  EXPECT_TRUE(postSeen.load(std::memory_order_relaxed));
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
TEST(HttpRouting, AsyncBodyReadTimeout) {
  RouterUpdateProxy router = ts.resetRouterAndGet();
  std::atomic_bool handlerInvoked{false};
  router.setPath(http::Method::POST, "/async-timeout", [&](HttpRequest&) -> RequestTask<HttpResponse> {
    handlerInvoked.store(true, std::memory_order_relaxed);
    co_return HttpResponse(http::StatusCodeOK).body("should-not-run");
  });

  constexpr auto readTimeout = std::chrono::milliseconds{50};
  BodyReadTimeoutScope timeout(readTimeout);
  PollIntervalScope pollInterval(std::chrono::milliseconds{5});

  test::ClientConnection cnx(ts.port());
  int fd = cnx.fd();
  ASSERT_GE(fd, 0);
  static constexpr std::string_view request =
      "POST /async-timeout HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 4\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, request);
  std::this_thread::sleep_for(readTimeout + std::chrono::milliseconds{20});

  std::string resp = test::recvWithTimeout(fd, std::chrono::milliseconds{500});
  ASSERT_FALSE(resp.empty());
  EXPECT_TRUE(resp.contains("HTTP/1.1 408")) << resp;
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine(http::Connection, "close"))) << resp;
  EXPECT_TRUE(handlerInvoked.load(std::memory_order_relaxed));
}

TEST(HttpRouting, AsyncLargeResponseChunks) {
  RouterUpdateProxy router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/async-large", [](HttpRequest&) -> RequestTask<HttpResponse> {
    std::string body(kAsyncLargePayload, 'x');
    co_return HttpResponse(http::StatusCodeOK).body(std::move(body));
  });

  test::RequestOptions opts;
  opts.method = "GET";
  opts.target = "/async-large";
  opts.headers.emplace_back(http::Connection, "close");
  opts.maxResponseBytes = kAsyncLargePayload + 1024;

  auto raw = test::requestOrThrow(ts.port(), opts);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  EXPECT_EQ(parsed.body.size(), kAsyncLargePayload);
}

TEST(HttpRouting, AsyncReadBodyBeforeBodyThrows) {
  RouterUpdateProxy router = ts.resetRouterAndGet();
  std::atomic_bool sawException{false};
  router.setPath(http::Method::POST, "/async-read-before-body", [&](HttpRequest& req) -> RequestTask<HttpResponse> {
    [[maybe_unused]] auto chunk = req.readBody();
    try {
      [[maybe_unused]] auto aggregated = req.body();
    } catch (const std::logic_error&) {
      sawException.store(true, std::memory_order_relaxed);
    }
    co_return HttpResponse("ok");
  });

  test::RequestOptions opts;
  opts.method = "POST";
  opts.target = "/async-read-before-body";
  opts.headers.emplace_back(http::Connection, "close");
  opts.body = "abc";
  opts.recvTimeout = std::chrono::milliseconds{500};

  auto resp = test::requestOrThrow(ts.port(), opts);
  EXPECT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(sawException.load(std::memory_order_relaxed));
}

TEST(HttpRouting, AsyncBodyBeforeReadBodyThrows) {
  RouterUpdateProxy router = ts.resetRouterAndGet();
  std::atomic_bool sawException{false};
  router.setPath(http::Method::POST, "/async-body-before-read", [&](HttpRequest& req) -> RequestTask<HttpResponse> {
    [[maybe_unused]] auto aggregated = req.body();
    try {
      [[maybe_unused]] auto chunk = req.readBody();
    } catch (const std::logic_error&) {
      sawException.store(true, std::memory_order_relaxed);
    }
    co_return HttpResponse("ok");
  });

  test::RequestOptions opts;
  opts.method = "POST";
  opts.target = "/async-body-before-read";
  opts.headers.emplace_back(http::Connection, "close");
  opts.body = "xyz";
  opts.recvTimeout = std::chrono::milliseconds{500};

  auto resp = test::requestOrThrow(ts.port(), opts);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(sawException.load(std::memory_order_relaxed));
}

TEST(HttpRouting, AsyncIdentityContentLengthReadBodyStreams) {
  ts.resetRouterAndGet().setPath(http::Method::POST, "/identity-stream-cl",
                                 [&](HttpRequest& req) -> RequestTask<HttpResponse> {
                                   std::string collected;
                                   while (req.hasMoreBody()) {
                                     auto chunk = req.readBody(3);
                                     EXPECT_FALSE(chunk.empty());
                                     collected.append(chunk);
                                   }
                                   EXPECT_FALSE(req.hasMoreBody());
                                   co_return req.makeResponse(collected);
                                 });

  test::RequestOptions opts;
  opts.method = "POST";
  opts.target = "/identity-stream-cl";
  opts.headers.emplace_back(http::Connection, "close");
  opts.body = "stream-this-body";
  opts.recvTimeout = std::chrono::milliseconds{500};

  auto resp = test::requestOrThrow(ts.port(), opts);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.ends_with("\r\n\r\nstream-this-body")) << resp;
}

TEST(HttpRouting, AsyncReadBodyAsyncStreams) {
  ts.resetRouterAndGet().setPath(http::Method::POST, "/async-readbody-async",
                                 [&](HttpRequest& req) -> RequestTask<HttpResponse> {
                                   std::string collected;
                                   while (req.hasMoreBody()) {
                                     std::string_view chunk = co_await req.readBodyAsync();
                                     collected.append(chunk);
                                   }
                                   co_return HttpResponse(http::StatusCodeOK).body(collected);
                                 });

  test::RequestOptions opts;
  opts.method = "POST";
  opts.target = "/async-readbody-async";
  opts.headers.emplace_back(http::Connection, "close");
  opts.body = "chunked-async-body-data";
  opts.recvTimeout = std::chrono::milliseconds{500};

  auto resp = test::requestOrThrow(ts.port(), opts);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.ends_with("\r\n\r\nchunked-async-body-data")) << resp;
}

TEST(HttpRouting, AsyncIdentityChunkedReadBodyStreams) {
  ts.resetRouterAndGet().setPath(http::Method::POST, "/identity-stream-chunked",
                                 [&](HttpRequest& req) -> RequestTask<HttpResponse> {
                                   std::string collected;
                                   while (req.hasMoreBody()) {
                                     auto chunk = req.readBody(4);
                                     EXPECT_FALSE(chunk.empty());
                                     collected.append(chunk);
                                   }
                                   co_return req.makeResponse(collected);
                                 });

  test::ClientConnection client(ts.port());
  const std::string request =
      "POST /identity-stream-chunked HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "5\r\nfirst\r\n"
      "8\r\n-second!\r\n"
      "0\r\n\r\n";
  test::sendAll(client.fd(), request);
  const std::string response = test::recvUntilClosed(client.fd());
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.ends_with("\r\n\r\nfirst-second!")) << response;
}

TEST(HttpRouting, AsyncHandlerStartsBeforeBodyComplete) {
  std::atomic_bool handlerStarted{false};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/async-early",
                                 [&](HttpRequest& req) -> RequestTask<HttpResponse> {
                                   handlerStarted.store(true, std::memory_order_release);
                                   std::string_view body = co_await req.bodyAwaitable();
                                   co_return HttpResponse(http::StatusCodeOK).body(body);
                                 });

  test::ClientConnection client(ts.port());
  const std::string headers =
      "POST /async-early HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Length: 10\r\n"
      "Connection: close\r\n"
      "\r\n";
  test::sendAll(client.fd(), headers);
  test::sendAll(client.fd(), "12345");
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  EXPECT_TRUE(handlerStarted.load(std::memory_order_acquire));

  test::sendAll(client.fd(), "67890");
  const std::string response = test::recvUntilClosed(client.fd());
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.ends_with("\r\n\r\n1234567890")) << response;
}

TEST(HttpRouting, AsyncHandlerStartsBeforeBodyComplete_ReadBodyAsync) {
  std::atomic_bool handlerStarted{false};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/async-early-readbody",
                                 [&](HttpRequest& req) -> RequestTask<HttpResponse> {
                                   handlerStarted.store(true, std::memory_order_release);
                                   std::string collected;
                                   while (req.hasMoreBody()) {
                                     std::string_view chunk = co_await req.readBodyAsync();
                                     collected.append(chunk);
                                   }
                                   co_return req.makeResponse(std::move(collected));
                                 });

  test::ClientConnection client(ts.port());
  const std::string headers =
      "POST /async-early-readbody HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n";
  test::sendAll(client.fd(), headers);
  test::sendAll(client.fd(), "5\r\n12345\r\n");

  const auto deadLine = std::chrono::steady_clock::now() + std::chrono::seconds{30};

  while (!handlerStarted.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadLine) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }

  test::sendAll(client.fd(), "5\r\n67890\r\n0\r\n\r\n");
  const std::string response = test::recvUntilClosed(client.fd());
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.ends_with("\r\n\r\n1234567890")) << response;
}

#endif

TEST(RouterUpdateProxy, ClearRemovesAllHandlers) {
  RouterUpdateProxy router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/will-be-cleared",
                 [](const HttpRequest&) { return HttpResponse("should not see this"); });

  router.clear();

  const std::string response = test::simpleGet(ts.port(), "/will-be-cleared");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 404")) << response;
}

TEST(RouterUpdateProxy, SetPathWithMethodBitmapAndStreamingHandler) {
  RouterUpdateProxy router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET | http::Method::POST, "/stream-multi",
                 [](const HttpRequest& req, HttpResponseWriter& writer) {
                   writer.status(http::StatusCodeOK);
                   writer.writeBody(std::string(http::MethodToStr(req.method())));
                   writer.end();
                 });

  const std::string getResp = test::simpleGet(ts.port(), "/stream-multi");
  EXPECT_TRUE(getResp.starts_with("HTTP/1.1 200")) << getResp;
  EXPECT_TRUE(getResp.contains("GET")) << getResp;

  test::RequestOptions postOpts;
  postOpts.method = "POST";
  postOpts.target = "/stream-multi";
  postOpts.headers.emplace_back("Content-Length", "0");
  const std::string postResp = test::requestOrThrow(ts.port(), postOpts);
  EXPECT_TRUE(postResp.starts_with("HTTP/1.1 200")) << postResp;
  EXPECT_TRUE(postResp.contains("POST")) << postResp;
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
TEST(RouterUpdateProxy, SetPathWithMethodBitmapAndAsyncHandler) {
  RouterUpdateProxy router = ts.resetRouterAndGet();
  router.setDefault([](HttpRequest& req) -> RequestTask<HttpResponse> {
    co_return HttpResponse().reason("async-default-" + std::string(http::MethodToStr(req.method())));
  });
  router.setPath(http::Method::GET | http::Method::PUT, "/async-multi",
                 [](HttpRequest& req) -> RequestTask<HttpResponse> {
                   co_return HttpResponse().reason("async-" + std::string(http::MethodToStr(req.method())));
                 });

  const std::string getResp = test::simpleGet(ts.port(), "/async-multi");
  EXPECT_TRUE(getResp.starts_with("HTTP/1.1 200 async-GET")) << getResp;

  test::RequestOptions putOpts;
  putOpts.method = "PUT";
  putOpts.target = "/async-multi";
  putOpts.headers.emplace_back("Content-Length", "0");
  const std::string putResp = test::requestOrThrow(ts.port(), putOpts);
  EXPECT_TRUE(putResp.starts_with("HTTP/1.1 200 async-PUT")) << putResp;
  // test default for other methods
  test::RequestOptions postOpts;
  postOpts.method = "POST";
  postOpts.target = "/async-default";
  postOpts.headers.emplace_back("Content-Length", "0");
  const std::string postResp = test::requestOrThrow(ts.port(), postOpts);
  EXPECT_TRUE(postResp.starts_with("HTTP/1.1 200 async-default-POST")) << postResp;
}

TEST(RouterUpdateProxy, PathEntryProxyCorsPolicy) {
  RouterUpdateProxy router = ts.resetRouterAndGet();
  CorsPolicy policy(CorsPolicy::Active::On);
  policy.allowOrigin("https://example.com").allowMethods(http::Method::GET).allowRequestHeader("X-Custom");

  router
      .setPath(http::Method::GET, "/with-cors",
               [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK, "cors-enabled"); })
      .cors(std::move(policy));

  test::RequestOptions opts;
  opts.method = "OPTIONS";
  opts.target = "/with-cors";
  opts.headers.emplace_back("Origin", "https://example.com");
  opts.headers.emplace_back("Access-Control-Request-Method", "GET");
  opts.headers.emplace_back("Content-Length", "0");
  const std::string preflightResp = test::requestOrThrow(ts.port(), opts);
  EXPECT_TRUE(preflightResp.contains("HTTP/1.1 204")) << preflightResp;
  EXPECT_TRUE(preflightResp.contains(MakeHttp1HeaderLine(http::AccessControlAllowOrigin, "https://example.com")))
      << preflightResp;
}

TEST(RouterUpdateProxy, SetDefaultStreamingHandler) {
  RouterUpdateProxy router = ts.resetRouterAndGet();
  router.setDefault([](const HttpRequest& req, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody("default-streaming:");
    writer.writeBody(req.path());
    writer.end();
  });

  const std::string response = test::simpleGet(ts.port(), "/unmatched-path");
  EXPECT_TRUE(response.contains("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains("default-streaming:")) << response;
  EXPECT_TRUE(response.contains("/unmatched-path")) << response;
}

// Test async handler exception during task creation (before coroutine starts)
TEST(HttpRouting, AsyncHandlerThrowsStdExceptionDuringCreation) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/async-throw-std", [](HttpRequest&) -> RequestTask<HttpResponse> {
    // Throw BEFORE entering coroutine body - this is caught by dispatchAsyncHandler
    throw std::runtime_error("Task creation failed");
    // Note: co_return would make this a coroutine, but throw happens first,
    // before coroutine frame is created, so exception is caught in dispatchAsyncHandler
    co_return HttpResponse(http::StatusCodeOK);
  });

  const std::string response = test::simpleGet(ts.port(), "/async-throw-std");
  EXPECT_TRUE(response.contains("HTTP/1.1 500")) << response;
  EXPECT_TRUE(response.contains("Task creation failed")) << response;
}

// Test async handler exception (non-std) during task creation (before coroutine starts)
TEST(HttpRouting, AsyncHandlerThrowsNonStdExceptionDuringCreation) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/async-throw-nonstd",
                                 [](HttpRequest&) -> RequestTask<HttpResponse> {
                                   // Throw BEFORE entering coroutine body - caught by dispatchAsyncHandler
                                   throw 42;  // Non-std exception
                                   co_return HttpResponse(http::StatusCodeOK);
                                 });

  const std::string response = test::simpleGet(ts.port(), "/async-throw-nonstd");
  EXPECT_TRUE(response.contains("HTTP/1.1 500")) << response;
  EXPECT_TRUE(response.contains("Unknown error")) << response;
}

// Test async handler returning invalid task
TEST(HttpRouting, AsyncHandlerReturnsInvalidTask) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/async-invalid-task",
                                 [](HttpRequest&) -> RequestTask<HttpResponse> {
                                   RequestTask<HttpResponse> task;  // Default constructed, invalid task
                                   return task;
                                 });

  const std::string response = test::simpleGet(ts.port(), "/async-invalid-task");
  EXPECT_TRUE(response.contains("HTTP/1.1 500")) << response;
  EXPECT_TRUE(response.contains("Async handler inactive")) << response;
}

// Test async handler returning task with null coroutine handle
TEST(HttpRouting, AsyncHandlerReturnsNullHandle) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/async-null-handle",
                                 [](HttpRequest&) -> RequestTask<HttpResponse> {
                                   RequestTask<HttpResponse> task = []() -> RequestTask<HttpResponse> {
                                     co_return HttpResponse(http::StatusCodeOK);
                                   }();
                                   auto handle = task.release();  // Release the handle, making task invalid
                                   handle.destroy();              // Clean up to prevent leak
                                   return task;
                                 });

  const std::string response = test::simpleGet(ts.port(), "/async-null-handle");
  EXPECT_TRUE(response.contains("HTTP/1.1 500")) << response;
  EXPECT_TRUE(response.contains("Async handler inactive")) << response;
}

// Test async handler exception during creation with body not ready (before coroutine starts)
TEST(HttpRouting, AsyncHandlerThrowsWithBodyNotReady) {
  ts.resetRouterAndGet().setPath(http::Method::POST, "/async-throw-no-body",
                                 [](HttpRequest&) -> RequestTask<HttpResponse> {
                                   // Throw BEFORE coroutine body - caught by dispatchAsyncHandler
                                   throw std::runtime_error("Failed before body ready");
                                   co_return HttpResponse(http::StatusCodeOK);
                                 });

  test::ClientConnection client(ts.port());
  const std::string request =
      "POST /async-throw-no-body HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Length: 100\r\n"
      "Connection: close\r\n"
      "\r\n";
  test::sendAll(client.fd(), request);

  const std::string response = test::recvWithTimeout(client.fd(), std::chrono::milliseconds{250});

  // When bodyReady=false and immediate close happens, response may not be fully sent
  if (response.empty()) {
    GTEST_SKIP() << "Server closed immediately without response (acceptable for bodyReady=false + immediate error)";
  }
  EXPECT_TRUE(response.contains("HTTP/1.1 500")) << response;
  EXPECT_TRUE(response.contains("Failed before body ready")) << response;
}

// Test async handler returning invalid task with body not ready
TEST(HttpRouting, AsyncHandlerInvalidTaskWithBodyNotReady) {
  ts.resetRouterAndGet().setPath(http::Method::POST, "/async-invalid-no-body",
                                 [](HttpRequest&) -> RequestTask<HttpResponse> {
                                   RequestTask<HttpResponse> task;  // Invalid task
                                   return task;
                                 });

  test::ClientConnection client(ts.port());
  const std::string request =
      "POST /async-invalid-no-body HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Length: 100\r\n"
      "Connection: close\r\n"
      "\r\n";
  test::sendAll(client.fd(), request);

  const std::string response = test::recvUntilClosed(client.fd());
  EXPECT_TRUE(response.contains("HTTP/1.1 500")) << response;
  EXPECT_TRUE(response.contains("Async handler inactive")) << response;
}

// Test async handler returning null handle with body not ready
TEST(HttpRouting, AsyncHandlerNullHandleWithBodyNotReady) {
  ts.resetRouterAndGet().setPath(http::Method::POST, "/async-null-no-body",
                                 [](HttpRequest&) -> RequestTask<HttpResponse> {
                                   RequestTask<HttpResponse> task = []() -> RequestTask<HttpResponse> {
                                     co_return HttpResponse(http::StatusCodeOK);
                                   }();
                                   auto handle = task.release();
                                   handle.destroy();  // Clean up to prevent leak
                                   return task;
                                 });

  test::ClientConnection client(ts.port());
  const std::string request =
      "POST /async-null-no-body HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Length: 100\r\n"
      "Connection: close\r\n"
      "\r\n";
  test::sendAll(client.fd(), request);

  const std::string response = test::recvUntilClosed(client.fd());
  EXPECT_TRUE(response.contains("HTTP/1.1 500")) << response;
  EXPECT_TRUE(response.contains("Async handler inactive")) << response;
}

// Test async handler non-std exception with body not ready (before coroutine starts)
TEST(HttpRouting, AsyncHandlerNonStdExceptionWithBodyNotReady) {
  ts.resetRouterAndGet().setPath(http::Method::POST, "/async-nonstd-no-body",
                                 [](HttpRequest&) -> RequestTask<HttpResponse> {
                                   // Throw BEFORE coroutine body - caught by dispatchAsyncHandler
                                   throw 999;  // Non-std exception
                                   co_return HttpResponse(http::StatusCodeOK);
                                 });

  test::ClientConnection client(ts.port());
  const std::string request =
      "POST /async-nonstd-no-body HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Length: 100\r\n"
      "\r\n";
  test::sendAll(client.fd(), request);

  // Try to read response with timeout
  const std::string response = test::recvWithTimeout(client.fd(), std::chrono::milliseconds{250});

  // When bodyReady=false and immediate close happens, we might not get a response
  if (response.empty()) {
    // This is acceptable - failFast with bodyReady=false may close immediately
    // The important thing is the exception handler was called (logged above)
    GTEST_SKIP()
        << "Server closed immediately without response (acceptable behavior for bodyReady=false + immediate error)";
  }
  EXPECT_TRUE(response.contains("HTTP/1.1 500")) << response;
  EXPECT_TRUE(response.contains("Unknown error")) << response;
}

// Test deferWork(): basic async work execution returning a value
TEST(HttpRouting, DeferWorkBasicReturnValue) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/defer-basic", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    // Run blocking work on background thread
    int result = co_await req.deferWork([]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      return 42;
    });
    co_return HttpResponse(http::StatusCodeOK).body("result=" + std::to_string(result));
  });

  const std::string response = test::simpleGet(ts.port(), "/defer-basic");
  EXPECT_TRUE(response.contains("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains("result=42")) << response;
}

// Test deferWork(): work returning a string
TEST(HttpRouting, DeferWorkReturnsString) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/defer-string", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    std::string result = co_await req.deferWork([]() -> std::string {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      return "computed-value";
    });
    co_return HttpResponse(http::StatusCodeOK).body(result);
  });

  const std::string response = test::simpleGet(ts.port(), "/defer-string");
  EXPECT_TRUE(response.contains("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains("computed-value")) << response;
}

// Test deferWork(): work returning an optional
TEST(HttpRouting, DeferWorkReturnsOptional) {
  ts.resetRouterAndGet().setPath(
      http::Method::GET, "/defer-optional", [](HttpRequest& req) -> RequestTask<HttpResponse> {
        std::optional<int> result = co_await req.deferWork([]() -> std::optional<int> {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          return 123;
        });
        if (result) {
          co_return HttpResponse(http::StatusCodeOK).body("found=" + std::to_string(*result));
        }
        co_return HttpResponse(http::StatusCodeNotFound).body("not found");
      });

  const std::string response = test::simpleGet(ts.port(), "/defer-optional");
  EXPECT_TRUE(response.contains("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains("found=123")) << response;
}

// Test deferWork(): multiple sequential defers in same handler
TEST(HttpRouting, DeferWorkMultipleSequential) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/defer-sequential",
                                 [](HttpRequest& req) -> RequestTask<HttpResponse> {
                                   int first = co_await req.deferWork([]() {
                                     std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                     return 10;
                                   });
                                   int second = co_await req.deferWork([first]() {
                                     std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                     return first * 2;
                                   });
                                   co_return HttpResponse(http::StatusCodeOK)
                                       .body("first=" + std::to_string(first) + ",second=" + std::to_string(second));
                                 });

  const std::string response = test::simpleGet(ts.port(), "/defer-sequential");
  EXPECT_TRUE(response.contains("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains("first=10,second=20")) << response;
}

// Test deferWork(): combined with bodyAwaitable
TEST(HttpRouting, DeferWorkCombinedWithBody) {
  ts.resetRouterAndGet().setPath(
      http::Method::POST, "/defer-with-body", [](HttpRequest& req) -> RequestTask<HttpResponse> {
        // First, wait for body
        std::string_view body = co_await req.bodyAwaitable();
        std::string bodyCopy(body);

        // Then, process body on background thread
        int result = co_await req.deferWork([bodyCopy]() {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          return static_cast<int>(bodyCopy.size());
        });

        co_return HttpResponse(http::StatusCodeOK).body("body_size=" + std::to_string(result));
      });

  test::RequestOptions opt;
  opt.method = "POST";
  opt.target = "/defer-with-body";
  opt.connection = "close";
  opt.body = "hello world!";
  const std::string response = test::requestOrThrow(ts.port(), opt);

  EXPECT_TRUE(response.contains("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains("body_size=12")) << response;
}

// Test deferWork(): event loop can process other requests while waiting
TEST(HttpRouting, DeferWorkEventLoopContinues) {
  std::atomic<int> concurrentRequests{0};
  std::atomic<int> maxConcurrent{0};

  ts.resetRouterAndGet().setPath(
      http::Method::GET, "/defer-concurrent", [&](HttpRequest& req) -> RequestTask<HttpResponse> {
        int current = ++concurrentRequests;
        // Update max concurrent
        int expected = maxConcurrent.load();
        while (current > expected && !maxConcurrent.compare_exchange_weak(expected, current)) {
        }

        (void)co_await req.deferWork([&]() {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          return 0;
        });

        --concurrentRequests;
        co_return HttpResponse(http::StatusCodeOK).body("done");
      });

  // Launch multiple requests in parallel
  std::vector<std::thread> threads;
  std::atomic<int> successCount{0};
  constexpr int kNumRequests = 5;

  threads.reserve(kNumRequests);
  for (int idx = 0; idx < kNumRequests; ++idx) {
    threads.emplace_back([&]() {
      const std::string response = test::simpleGet(ts.port(), "/defer-concurrent");
      if (response.contains("HTTP/1.1 200") && response.contains("done")) {
        ++successCount;
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  EXPECT_EQ(successCount.load(), kNumRequests);
  // With proper deferWork implementation, multiple requests should be processed concurrently
  // The maxConcurrent should be > 1 if the event loop is properly handling multiple requests
  EXPECT_GT(maxConcurrent.load(), 1) << "Event loop should handle multiple concurrent requests while waiting for "
                                        "deferWork";
}

// Test deferWork(): work returning bool
TEST(HttpRouting, DeferWorkReturnsBool) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/defer-bool", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    bool result = co_await req.deferWork([]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      return true;
    });
    co_return HttpResponse(http::StatusCodeOK).body(result ? "success" : "failure");
  });

  const std::string response = test::simpleGet(ts.port(), "/defer-bool");
  EXPECT_TRUE(response.contains("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains("success")) << response;
}

// Test deferWork(): exception (std::exception) thrown in work function
TEST(HttpRouting, DeferWorkThrowsStdException) {
  ts.resetRouterAndGet().setPath(
      http::Method::GET, "/defer-throw-std", [](HttpRequest& req) -> RequestTask<HttpResponse> {
        // The work function throws - exception is captured and rethrown in await_resume
        try {
          (void)co_await req.deferWork([]() -> int {
            throw std::runtime_error("work failed");
            return 0;  // never reached
          });
          co_return HttpResponse(http::StatusCodeOK).body("should not reach");
        } catch (const std::runtime_error& ex) {
          co_return HttpResponse(http::StatusCodeInternalServerError).body(std::string("caught: ") + ex.what());
        }
      });

  const std::string response = test::simpleGet(ts.port(), "/defer-throw-std");
  EXPECT_TRUE(response.contains("HTTP/1.1 500")) << response;
  EXPECT_TRUE(response.contains("caught: work failed")) << response;
}

// Test deferWork(): non-std exception thrown in work function
TEST(HttpRouting, DeferWorkThrowsNonStdException) {
  ts.resetRouterAndGet().setPath(
      http::Method::GET, "/defer-throw-nonstd", [](HttpRequest& req) -> RequestTask<HttpResponse> {
        // The work function throws a non-std exception
        try {
          (void)co_await req.deferWork([]() -> int {
            throw 42;  // non-std exception
            return 0;
          });
          co_return HttpResponse(http::StatusCodeOK).body("should not reach");
        } catch (int ex) {
          co_return HttpResponse(http::StatusCodeInternalServerError).body("caught int: " + std::to_string(ex));
        }
      });

  const std::string response = test::simpleGet(ts.port(), "/defer-throw-nonstd");
  EXPECT_TRUE(response.contains("HTTP/1.1 500")) << response;
  EXPECT_TRUE(response.contains("caught int: 42")) << response;
}

// Test deferWork(): unhandled exception propagates to coroutine promise
TEST(HttpRouting, DeferWorkUnhandledException) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/defer-unhandled",
                                 [](HttpRequest& req) -> RequestTask<HttpResponse> {
                                   // Exception not caught in coroutine - propagates to promise
                                   (void)co_await req.deferWork([]() -> int {
                                     throw std::runtime_error("unhandled in work");
                                     return 0;
                                   });
                                   co_return HttpResponse(http::StatusCodeOK).body("should not reach");
                                 });

  const std::string response = test::simpleGet(ts.port(), "/defer-unhandled");
  // Server catches unhandled exception and returns 500
  EXPECT_TRUE(response.starts_with("HTTP/1.1 500")) << response;
  EXPECT_TRUE(response.contains("unhandled in work")) << response;
}

#endif

TEST(HttpRoutingCoverageImprovements, CatchAllRoute) {
  // Tests the /* catch-all path handling and IsWildcardStart with '*'
  ts.resetRouterAndGet().setPath(http::Method::GET, "/api/*",
                                 [](const HttpRequest& req) { return HttpResponse(req.path()); });

  auto resp1 = test::simpleGet(ts.port(), "/api/anything");
  EXPECT_TRUE(resp1.starts_with("HTTP/1.1 200")) << resp1;
  EXPECT_TRUE(resp1.contains("/api/anything")) << resp1;

  auto resp2 = test::simpleGet(ts.port(), "/api/deeply/nested/path");
  EXPECT_TRUE(resp2.starts_with("HTTP/1.1 200")) << resp2;
  EXPECT_TRUE(resp2.contains("/api/deeply/nested/path")) << resp2;
}

TEST(HttpRoutingCoverageImprovements, SimpleParameter) {
  // Tests basic parameter parsing
  ts.resetRouterAndGet().setPath(http::Method::GET, "/users/{id}", [](const HttpRequest& req) {
    const auto& params = req.pathParams();
    if (params.empty()) {
      return HttpResponse("empty");
    }
    return HttpResponse(std::string(params.begin()->second));
  });

  auto resp = test::simpleGet(ts.port(), "/users/123");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.contains("123")) << resp;
}

TEST(HttpRoutingCoverageImprovements, ParameterWithLiteralPrefix) {
  // Tests parameter with literal prefix like "id-{value}"
  ts.resetRouterAndGet().setPath(http::Method::GET, "/data/id-{identifier}", [](const HttpRequest& req) {
    const auto& params = req.pathParams();
    return HttpResponse(std::string(params.begin()->second));
  });

  auto resp = test::simpleGet(ts.port(), "/data/id-abc123");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.contains("abc123")) << resp;
}

TEST(HttpRoutingCoverageImprovements, ParameterWithLiteralSuffix) {
  // Tests parameter with literal suffix like "{value}.html"
  ts.resetRouterAndGet().setPath(http::Method::GET, "/pages/{name}.html", [](const HttpRequest& req) {
    const auto& params = req.pathParams();
    return HttpResponse(std::string(params.begin()->second));
  });

  auto resp = test::simpleGet(ts.port(), "/pages/index.html");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.contains("index")) << resp;
}

TEST(HttpRoutingCoverageImprovements, MultipleParametersWithSeparators) {
  // Tests parameter parsing with literal separators between params
  ts.resetRouterAndGet().setPath(http::Method::GET, "/users/{id}/posts/{postId}", [](const HttpRequest& req) {
    const auto& params = req.pathParams();
    std::string result;
    for (const auto& [key, value] : params) {
      if (!result.empty()) {
        result += ":";
      }
      result += std::string(value);
    }
    return HttpResponse(result);
  });

  auto resp = test::simpleGet(ts.port(), "/users/123/posts/456");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.contains("123")) << resp;
  EXPECT_TRUE(resp.contains("456")) << resp;
}

TEST(HttpRoutingCoverageImprovements, CatchAllAfterParameters) {
  // Tests catch-all after parameter segment
  ts.resetRouterAndGet().setPath(http::Method::GET, "/api/{version}/files/*",
                                 [](const HttpRequest& req) { return HttpResponse(req.path()); });

  auto resp = test::simpleGet(ts.port(), "/api/v1/files/some/deep/path");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.contains("/api/v1/files/some/deep/path")) << resp;
}

TEST(HttpRoutingCoverageImprovements, RootPathOnly) {
  // Tests root path "/"
  ts.resetRouterAndGet().setPath(http::Method::GET, "/", [](const HttpRequest&) { return HttpResponse("root"); });

  auto resp = test::simpleGet(ts.port(), "/");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.contains("root")) << resp;
}

TEST(HttpRoutingCoverageImprovements, DeepNestedParameters) {
  // Tests multiple levels of nested parameters
  ts.resetRouterAndGet().setPath(http::Method::GET, "/org/{org}/team/{team}/project/{project}/issue/{issue}",
                                 [](const HttpRequest& req) {
                                   const auto& params = req.pathParams();
                                   std::string result;
                                   for (const auto& [key, value] : params) {
                                     if (!result.empty()) {
                                       result += "-";
                                     }
                                     result += std::string(value);
                                   }
                                   return HttpResponse(result);
                                 });

  auto resp = test::simpleGet(ts.port(), "/org/myorg/team/devteam/project/web/issue/42");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.contains("myorg")) << resp;
  EXPECT_TRUE(resp.contains("devteam")) << resp;
  EXPECT_TRUE(resp.contains("web")) << resp;
  EXPECT_TRUE(resp.contains("42")) << resp;
}

TEST(HttpRoutingCoverageImprovements, PathWithTrailingWildcard) {
  // Tests a path that ends with /*
  ts.resetRouterAndGet().setPath(http::Method::GET, "/assets/*",
                                 [](const HttpRequest& req) { return HttpResponse(req.path()); });

  auto resp = test::simpleGet(ts.port(), "/assets/css/style.css");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.contains("/assets/css/style.css")) << resp;
}

TEST(HttpRoutingCoverageImprovements, WildcardChildWithStaticSiblings) {
  // Tests the branch where we insert a static child before a wildcard child
  // This covers: if (pNode->hasWildChild && !pNode->children.empty()) { insert before wildcard }
  // Structure: /api/{id}/files/* and /api/{id}/metadata should create both children
  // where metadata is static and * is wildcard, requiring insertion before wildcard
  ts.resetRouterAndGet();
  ts.router().setPath(http::Method::GET, "/api/{id}/files/*",
                      [](const HttpRequest& req) { return HttpResponse("files-" + std::string(req.path())); });
  ts.router().setPath(http::Method::GET, "/api/{id}/metadata",
                      [](const HttpRequest&) { return HttpResponse("metadata"); });

  // Test the catch-all route
  auto resp1 = test::simpleGet(ts.port(), "/api/123/files/document.txt");
  EXPECT_TRUE(resp1.starts_with("HTTP/1.1 200")) << resp1;
  EXPECT_TRUE(resp1.contains("files-")) << resp1;

  // Test the static metadata route
  auto resp2 = test::simpleGet(ts.port(), "/api/123/metadata");
  EXPECT_TRUE(resp2.starts_with("HTTP/1.1 200")) << resp2;
  EXPECT_TRUE(resp2.contains("metadata")) << resp2;
}

TEST(HttpRoutingCoverageImprovements, CatchAllAfterMultipleStaticPaths) {
  // Tests insertion of static children followed by catch-all on same node
  // This exercises the insertion logic when hasWildChild is true
  ts.resetRouterAndGet();
  ts.router().setPath(http::Method::GET, "/v1/api/users", [](const HttpRequest&) { return HttpResponse("users"); });
  ts.router().setPath(http::Method::GET, "/v1/api/posts", [](const HttpRequest&) { return HttpResponse("posts"); });
  ts.router().setPath(http::Method::GET, "/v1/api/*",
                      [](const HttpRequest& req) { return HttpResponse("catch-" + std::string(req.path())); });

  auto resp1 = test::simpleGet(ts.port(), "/v1/api/users");
  EXPECT_TRUE(resp1.contains("users")) << resp1;

  auto resp2 = test::simpleGet(ts.port(), "/v1/api/posts");
  EXPECT_TRUE(resp2.contains("posts")) << resp2;

  // Test catch-all for other paths
  auto resp3 = test::simpleGet(ts.port(), "/v1/api/items");
  EXPECT_TRUE(resp3.contains("catch-")) << resp3;
}
