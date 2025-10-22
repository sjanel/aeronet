#include "aeronet/router.hpp"

#include <gtest/gtest.h>

#include <cstddef>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/router-config.hpp"
#include "exception.hpp"

using namespace aeronet;

TEST(RouterTest, RegisterAndMatchNormalHandler) {
  Router router;

  bool called = false;
  router.setPath(http::Method::GET, "/hello", [&called](const HttpRequest &) {
    called = true;
    return HttpResponse(200, "OK");
  });

  auto res = router.match(http::Method::GET, "/hello");
  ASSERT_NE(res.requestHandler, nullptr);
  ASSERT_EQ(res.streamingHandler, nullptr);
  ASSERT_FALSE(res.methodNotAllowed);

  // Invoke the handler via the pointer to ensure it is callable and behaves correctly
  // Use an aligned storage because HttpRequest constructor should be kept private
  alignas(HttpRequest) std::byte httpRequestStorage[sizeof(HttpRequest)];
  const HttpRequest &dummy = *reinterpret_cast<const HttpRequest *>(&httpRequestStorage);
  HttpResponse resp = (*res.requestHandler)(dummy);
  EXPECT_EQ(resp.statusCode(), 200);
  EXPECT_TRUE(called);
}

TEST(RouterTest, RegisterAndMatchStreamingHandler) {
  Router router;

  bool streamCalled = false;
  router.setPath(http::Method::POST, "/stream",
                 [&streamCalled](const HttpRequest &, [[maybe_unused]] HttpResponseWriter &) { streamCalled = true; });

  auto res = router.match(http::Method::POST, "/stream");
  ASSERT_EQ(res.requestHandler, nullptr);
  ASSERT_NE(res.streamingHandler, nullptr);
  ASSERT_FALSE(res.methodNotAllowed);

  // We cannot easily construct an HttpResponseWriter here without a real HttpServer.
  // Verifying non-null streamingHandler is sufficient for the Router::match contract.
  EXPECT_FALSE(streamCalled);
}

TEST(RouterTest, MethodNotAllowedAndFallback) {
  Router router;

  router.setPath(http::Method::GET, "/onlyget", [](const HttpRequest &) { return HttpResponse(200); });

  // POST should result in methodNotAllowed
  auto resPost = router.match(http::Method::POST, "/onlyget");
  EXPECT_TRUE(resPost.methodNotAllowed);
  EXPECT_EQ(resPost.requestHandler, nullptr);

  // GET should match
  auto resGet = router.match(http::Method::GET, "/onlyget");
  EXPECT_FALSE(resGet.methodNotAllowed);
  EXPECT_NE(resGet.requestHandler, nullptr);

  // No path registered -> fallback to no handler (empty)
  auto resMissing = router.match(http::Method::GET, "/missing");
  EXPECT_EQ(resMissing.requestHandler, nullptr);
  EXPECT_EQ(resMissing.streamingHandler, nullptr);
  EXPECT_FALSE(resMissing.methodNotAllowed);
}

TEST(RouterTest, GlobalDefaultHandlersUsedWhenNoPath) {
  Router router;

  router.setDefault([](const HttpRequest &) { return HttpResponse(204); });

  auto res = router.match(http::Method::GET, "/nope");
  ASSERT_NE(res.requestHandler, nullptr);
  ASSERT_EQ(res.streamingHandler, nullptr);
  EXPECT_FALSE(res.methodNotAllowed);

  // streaming default
  Router r2;
  bool sCalled = false;
  r2.setDefault([&sCalled](const HttpRequest &, HttpResponseWriter &writerParam) {
    sCalled = true;
    writerParam.end();
  });
  auto res2 = r2.match(http::Method::GET, "/nope");
  ASSERT_EQ(res2.requestHandler, nullptr);
  ASSERT_NE(res2.streamingHandler, nullptr);
}

TEST(RouterTest, TrailingSlashRedirectAndNormalize) {
  // Redirect policy: registering /p should redirect /p/ -> AddSlash or RemoveSlash depending
  RouterConfig cfg;
  cfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect);
  Router router(cfg);

  router.setPath(http::Method::GET, "/can", [](const HttpRequest &) { return HttpResponse(200); });

  // exact match
  auto resExact = router.match(http::Method::GET, "/can");
  EXPECT_NE(resExact.requestHandler, nullptr);
  EXPECT_EQ(resExact.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::None);

  // non-exact with trailing slash should request redirect (RemoveSlash)
  auto resSlashed = router.match(http::Method::GET, "/can/");
  EXPECT_EQ(resSlashed.requestHandler, nullptr);
  EXPECT_EQ(resSlashed.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::RemoveSlash);
}

TEST(RouterTest, HeadFallbackToGet) {
  Router router;
  router.setPath(http::Method::GET, "/hf", [](const HttpRequest &) { return HttpResponse(200); });

  // HEAD should fallback to GET handler when no explicit HEAD handler registered
  auto resHead = router.match(http::Method::HEAD, "/hf");
  EXPECT_NE(resHead.requestHandler, nullptr);
  EXPECT_EQ(resHead.streamingHandler, nullptr);
  EXPECT_FALSE(resHead.methodNotAllowed);
}

TEST(RouterTest, MethodMergingAndOverwrite) {
  Router router;
  // register GET and then add POST using method-bmp OR
  router.setPath(http::Method::GET, "/merge", [](const HttpRequest &) { return HttpResponse(200); });
  router.setPath(http::Method::POST, "/merge", [](const HttpRequest &) { return HttpResponse(201); });

  auto rGet = router.match(http::Method::GET, "/merge");
  EXPECT_NE(rGet.requestHandler, nullptr);
  EXPECT_FALSE(rGet.methodNotAllowed);

  auto rPost = router.match(http::Method::POST, "/merge");
  EXPECT_NE(rPost.requestHandler, nullptr);
  EXPECT_FALSE(rPost.methodNotAllowed);
}

TEST(RouterTest, StreamingVsNormalConflictThrows) {
  Router router;
  router.setPath(http::Method::GET, "/conf", [](const HttpRequest &) { return HttpResponse(200); });
  // Attempting to register a streaming handler for the same path+method should throw
  EXPECT_THROW(router.setPath(http::Method::GET, std::string{"/conf"},
                              Router::StreamingHandler([](const HttpRequest &, HttpResponseWriter &) {})),
               aeronet::exception);
}

TEST(RouterTest, TrailingSlashStrictAndNormalize) {
  // Strict: /a/ registered does not match /a
  RouterConfig cfgStrict;
  cfgStrict.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Strict);
  Router rStrict(cfgStrict);
  rStrict.setPath(http::Method::GET, "/s/", [](const HttpRequest &) { return HttpResponse(200); });
  auto res1 = rStrict.match(http::Method::GET, "/s/");
  EXPECT_NE(res1.requestHandler, nullptr);
  auto res1b = rStrict.match(http::Method::GET, "/s");
  EXPECT_EQ(res1b.requestHandler, nullptr);

  // Normalize: registering /n/ makes /n acceptable
  RouterConfig cfgNorm;
  cfgNorm.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Normalize);
  Router rNorm(cfgNorm);
  rNorm.setPath(http::Method::GET, "/n/", [](const HttpRequest &) { return HttpResponse(200); });
  auto res2 = rNorm.match(http::Method::GET, "/n");
  EXPECT_NE(res2.requestHandler, nullptr);
}
