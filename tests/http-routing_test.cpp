#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/request-task.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

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
  ts.router().setPath(http::Method::GET, "/hello",
                      [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK, "OK").body("world"); });
  ts.router().setPath(http::Method::GET | http::Method::POST, "/multi", [](const HttpRequest& req) {
    return HttpResponse(http::StatusCodeOK, "OK").body(std::string(http::MethodToStr(req.method())) + "!");
  });

  test::RequestOptions getHello;
  getHello.method = "GET";
  getHello.target = "/hello";
  auto resp1 = test::requestOrThrow(ts.port(), getHello);
  EXPECT_TRUE(resp1.contains("200 OK"));
  EXPECT_TRUE(resp1.contains("world"));
  test::RequestOptions postHello;
  postHello.method = "POST";
  postHello.target = "/hello";
  postHello.headers.emplace_back("Content-Length", "0");
  auto resp2 = test::requestOrThrow(ts.port(), postHello);
  EXPECT_TRUE(resp2.contains("405 Method Not Allowed"));
  test::RequestOptions getMissing;
  getMissing.method = "GET";
  getMissing.target = "/missing";
  auto resp3 = test::requestOrThrow(ts.port(), getMissing);
  EXPECT_TRUE(resp3.contains("404 Not Found"));
  test::RequestOptions postMulti;
  postMulti.method = "POST";
  postMulti.target = "/multi";
  postMulti.headers.emplace_back("Content-Length", "0");
  auto resp4 = test::requestOrThrow(ts.port(), postMulti);
  EXPECT_TRUE(resp4.contains("200 OK"));
  EXPECT_TRUE(resp4.contains("POST!"));
}

TEST(HttpRouting, AsyncHandlerDispatch) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/async-route", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    std::string payload("async:");
    payload.append(req.path());
    co_return HttpResponse(http::StatusCodeOK, http::ReasonOK).body(std::move(payload));
  });

  const std::string response = test::simpleGet(ts.port(), "/async-route");
  EXPECT_TRUE(response.contains("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains("async:/async-route")) << response;
}

TEST(HttpRouting, GlobalFallbackWithPathHandlers) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(200, "OK"); });
  // Adding path handler after global handler is now allowed (Phase 2 mixing model)
  EXPECT_NO_THROW(ts.router().setPath(http::Method::GET, "/x", [](const HttpRequest&) { return HttpResponse(200); }));
}

TEST(HttpRouting, PathParametersInjectedIntoRequest) {
  std::string seenUser;
  std::string seenPost;
  ts.router().setPath(http::Method::GET, "/users/{userId}/posts/{postId}", [&](const HttpRequest& req) {
    const auto& params = req.pathParams();
    if (const auto itUser = params.find("userId"); itUser != params.end()) {
      seenUser.assign(itUser->second);
    }
    if (const auto itPost = params.find("postId"); itPost != params.end()) {
      seenPost.assign(itPost->second);
    }
    return HttpResponse(200, "OK").body("ok");
  });

  test::RequestOptions reqOpts;
  reqOpts.method = "GET";
  reqOpts.target = "/users/42/posts/abcd";
  auto resp = test::requestOrThrow(ts.server.port(), reqOpts);
  EXPECT_TRUE(resp.contains("200 OK"));
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
  ASSERT_TRUE(resp.contains("Location: /gamma\r\n"));
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
  ASSERT_TRUE(redirect.contains("Location: /redir\r\n"));
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
    HttpResponse resp;
    resp.body("handler");
    return resp;
  });

  ts.router().addResponseMiddleware(
      [](const HttpRequest&, HttpResponse& resp) { resp.header("X-Global-Middleware", "applied"); });

  ts.router().addRequestMiddleware([](HttpRequest& req) {
    if (req.path() == "/mw-short") {
      HttpResponse resp(http::StatusCodeServiceUnavailable, "Service Unavailable");
      resp.body("short-circuited");
      return MiddlewareResult::ShortCircuit(std::move(resp));
    }
    return MiddlewareResult::Continue();
  });

  const std::string response = test::simpleGet(ts.port(), "/mw-short");
  EXPECT_TRUE(response.contains("HTTP/1.1 503")) << response;
  EXPECT_TRUE(response.contains("short-circuited")) << response;
  EXPECT_TRUE(response.contains("X-Global-Middleware: applied")) << response;
  EXPECT_FALSE(handlerCalled.load(std::memory_order_relaxed));
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
    const std::string existingBody(resp.body());
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
        writer.status(http::StatusCodeOK, http::ReasonOK);
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
    resp.status(http::StatusCodeAccepted, "Accepted by middleware");
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
  std::vector<HttpServer::MiddlewareMetrics> captured;
  std::vector<std::string> requestPaths;
  ts.server.setMiddlewareMetricsCallback([&](const HttpServer::MiddlewareMetrics& metrics) {
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

  std::vector<HttpServer::MiddlewareMetrics> metrics;
  {
    std::scoped_lock lock(metricsMutex);
    metrics = captured;
  }

  ASSERT_EQ(metrics.size(), 4U);

  EXPECT_EQ(metrics[0].phase, HttpServer::MiddlewareMetrics::Phase::Pre);
  EXPECT_TRUE(metrics[0].isGlobal);
  EXPECT_FALSE(metrics[0].shortCircuited);
  EXPECT_FALSE(metrics[0].threw);
  EXPECT_FALSE(metrics[0].streaming);
  EXPECT_EQ(metrics[0].index, 0U);
  EXPECT_EQ(metrics[0].method, http::Method::GET);
  EXPECT_EQ(requestPaths[0], "/mw-metrics");

  EXPECT_EQ(metrics[1].phase, HttpServer::MiddlewareMetrics::Phase::Pre);
  EXPECT_FALSE(metrics[1].isGlobal);
  EXPECT_FALSE(metrics[1].shortCircuited);
  EXPECT_FALSE(metrics[1].threw);
  EXPECT_FALSE(metrics[1].streaming);
  EXPECT_EQ(metrics[1].index, 0U);
  EXPECT_EQ(requestPaths[1], "/mw-metrics");

  EXPECT_EQ(metrics[2].phase, HttpServer::MiddlewareMetrics::Phase::Post);
  EXPECT_FALSE(metrics[2].isGlobal);
  EXPECT_FALSE(metrics[2].shortCircuited);
  EXPECT_FALSE(metrics[2].threw);
  EXPECT_FALSE(metrics[2].streaming);
  EXPECT_EQ(metrics[2].index, 0U);

  EXPECT_EQ(metrics[3].phase, HttpServer::MiddlewareMetrics::Phase::Post);
  EXPECT_TRUE(metrics[3].isGlobal);
  EXPECT_FALSE(metrics[3].shortCircuited);
  EXPECT_FALSE(metrics[3].threw);
  EXPECT_FALSE(metrics[3].streaming);
  EXPECT_EQ(metrics[3].index, 0U);
}

TEST(HttpMiddlewareMetrics, MarksShortCircuit) {
  std::mutex metricsMutex;
  std::vector<HttpServer::MiddlewareMetrics> captured;
  ts.server.setMiddlewareMetricsCallback([&](const HttpServer::MiddlewareMetrics& metrics) {
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
    HttpResponse resp(http::StatusCodeServiceUnavailable, "blocked");
    resp.body("shorted");
    return MiddlewareResult::ShortCircuit(std::move(resp));
  });
  entry.after([](const HttpRequest&, HttpResponse&) {});

  const std::string response = test::simpleGet(ts.port(), "/mw-short-metrics");
  ASSERT_TRUE(response.contains("HTTP/1.1 503")) << response;
  ASSERT_FALSE(handlerInvoked.load(std::memory_order_relaxed));

  ts.server.setMiddlewareMetricsCallback({});

  std::vector<HttpServer::MiddlewareMetrics> metrics;
  {
    std::scoped_lock lock(metricsMutex);
    metrics = captured;
  }

  ASSERT_EQ(metrics.size(), 4U);
  EXPECT_EQ(metrics[0].phase, HttpServer::MiddlewareMetrics::Phase::Pre);
  EXPECT_TRUE(metrics[0].isGlobal);
  EXPECT_FALSE(metrics[0].shortCircuited);
  EXPECT_FALSE(metrics[0].streaming);

  EXPECT_EQ(metrics[1].phase, HttpServer::MiddlewareMetrics::Phase::Pre);
  EXPECT_FALSE(metrics[1].isGlobal);
  EXPECT_TRUE(metrics[1].shortCircuited);
  EXPECT_FALSE(metrics[1].streaming);

  EXPECT_EQ(metrics[2].phase, HttpServer::MiddlewareMetrics::Phase::Post);
  EXPECT_FALSE(metrics[2].isGlobal);
  EXPECT_FALSE(metrics[2].shortCircuited);
  EXPECT_FALSE(metrics[2].streaming);

  EXPECT_EQ(metrics[3].phase, HttpServer::MiddlewareMetrics::Phase::Post);
  EXPECT_TRUE(metrics[3].isGlobal);
  EXPECT_FALSE(metrics[3].shortCircuited);
  EXPECT_FALSE(metrics[3].streaming);
}

TEST(HttpMiddlewareMetrics, StreamingFlagPropagates) {
  std::mutex metricsMutex;
  std::vector<HttpServer::MiddlewareMetrics> captured;
  std::vector<std::string> requestPaths;
  ts.server.setMiddlewareMetricsCallback([&](const HttpServer::MiddlewareMetrics& metrics) {
    std::scoped_lock lock(metricsMutex);
    captured.push_back(metrics);
    requestPaths.emplace_back(metrics.requestPath);
  });

  RouterUpdateProxy router = ts.resetRouterAndGet();
  router.addRequestMiddleware([](HttpRequest&) { return MiddlewareResult::Continue(); });
  router.addResponseMiddleware([](const HttpRequest&, HttpResponse&) {});

  auto entry =
      router.setPath(http::Method::GET, "/mw-stream-metrics", [](const HttpRequest&, HttpResponseWriter& writer) {
        writer.status(http::StatusCodeOK, http::ReasonOK);
        writer.header("X", "1");
        EXPECT_TRUE(writer.writeBody("chunk"));
        writer.end();
      });

  entry.before([](HttpRequest&) { return MiddlewareResult::Continue(); });
  entry.after([](const HttpRequest&, HttpResponse&) {});

  const std::string response = test::simpleGet(ts.port(), "/mw-stream-metrics");
  ASSERT_TRUE(response.contains("HTTP/1.1 200")) << response;

  ts.server.setMiddlewareMetricsCallback({});

  std::vector<HttpServer::MiddlewareMetrics> metrics;
  {
    std::scoped_lock lock(metricsMutex);
    metrics = captured;
  }

  ASSERT_EQ(metrics.size(), 4U);
  for (const auto& metric : metrics) {
    EXPECT_TRUE(metric.streaming);
  }
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
  EXPECT_TRUE(resp.contains("Connection: close")) << resp;
  EXPECT_TRUE(handlerInvoked.load(std::memory_order_relaxed));
}

TEST(HttpRouting, AsyncLargeResponseChunks) {
  RouterUpdateProxy router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/async-large", [](HttpRequest&) -> RequestTask<HttpResponse> {
    std::string body(kAsyncLargePayload, 'x');
    co_return HttpResponse(http::StatusCodeOK, http::ReasonOK).body(std::move(body));
  });

  test::RequestOptions opts;
  opts.method = "GET";
  opts.target = "/async-large";
  opts.headers.emplace_back("Connection", "close");
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
    (void)chunk;
    try {
      [[maybe_unused]] auto aggregated = req.body();
      (void)aggregated;
    } catch (const std::logic_error&) {
      sawException.store(true, std::memory_order_relaxed);
    }
    co_return HttpResponse(http::StatusCodeOK).body("ok");
  });

  test::RequestOptions opts;
  opts.method = "POST";
  opts.target = "/async-read-before-body";
  opts.headers.emplace_back("Connection", "close");
  opts.body = "abc";

  auto resp = test::requestOrThrow(ts.port(), opts);
  EXPECT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(sawException.load(std::memory_order_relaxed));
}

TEST(HttpRouting, AsyncBodyBeforeReadBodyThrows) {
  RouterUpdateProxy router = ts.resetRouterAndGet();
  std::atomic_bool sawException{false};
  router.setPath(http::Method::POST, "/async-body-before-read", [&](HttpRequest& req) -> RequestTask<HttpResponse> {
    [[maybe_unused]] auto aggregated = req.body();
    (void)aggregated;
    try {
      [[maybe_unused]] auto chunk = req.readBody();
      (void)chunk;
    } catch (const std::logic_error&) {
      sawException.store(true, std::memory_order_relaxed);
    }
    co_return HttpResponse(http::StatusCodeOK).body("ok");
  });

  test::RequestOptions opts;
  opts.method = "POST";
  opts.target = "/async-body-before-read";
  opts.headers.emplace_back("Connection", "close");
  opts.body = "xyz";

  auto resp = test::requestOrThrow(ts.port(), opts);
  EXPECT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
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
                                   co_return HttpResponse(http::StatusCodeOK).body(collected);
                                 });

  test::RequestOptions opts;
  opts.method = "POST";
  opts.target = "/identity-stream-cl";
  opts.headers.emplace_back("Connection", "close");
  opts.body = "stream-this-body";

  auto resp = test::requestOrThrow(ts.port(), opts);
  EXPECT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.contains("stream-this-body")) << resp;
}

TEST(HttpRouting, AsyncReadBodyAsyncStreams) {
  ts.resetRouterAndGet().setPath(http::Method::POST, "/async-readbody-async",
                                 [&](HttpRequest& req) -> RequestTask<HttpResponse> {
                                   std::string collected;
                                   while (req.hasMoreBody()) {
                                     std::string_view chunk = co_await req.readBodyAsync();
                                     if (chunk.empty()) break;
                                     collected.append(chunk);
                                   }
                                   co_return HttpResponse(http::StatusCodeOK).body(collected);
                                 });

  test::RequestOptions opts;
  opts.method = "POST";
  opts.target = "/async-readbody-async";
  opts.headers.emplace_back("Connection", "close");
  opts.body = "chunked-async-body-data";

  auto resp = test::requestOrThrow(ts.port(), opts);
  EXPECT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.find("chunked-async-body-data") != std::string::npos) << resp;
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
                                   co_return HttpResponse(http::StatusCodeOK).body(collected);
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
  EXPECT_TRUE(response.contains("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains("first-second!")) << response;
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
  EXPECT_TRUE(response.contains("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains("1234567890")) << response;
}
