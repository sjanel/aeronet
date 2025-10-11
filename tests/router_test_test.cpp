#include <gtest/gtest.h>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router.hpp"
#include "exception.hpp"

using namespace aeronet;

TEST(RouterUnitTest, RegisterAndMatchNormalHandler) {
  Router router;

  bool called = false;
  router.setPath("/hello", http::Method::GET, [&called](const HttpRequest&) {
    called = true;
    return HttpResponse(200, "OK");
  });

  auto res = router.match(http::Method::GET, "/hello");
  ASSERT_NE(res.requestHandler, nullptr);
  ASSERT_EQ(res.streamingHandler, nullptr);
  ASSERT_FALSE(res.methodNotAllowed);

  // Invoke the handler via the pointer to ensure it is callable and behaves
  HttpRequest dummy;
  HttpResponse resp = (*res.requestHandler)(dummy);
  EXPECT_EQ(resp.statusCode(), 200);
  EXPECT_TRUE(called);
}

TEST(RouterUnitTest, RegisterAndMatchStreamingHandler) {
  Router router;

  bool streamCalled = false;
  router.setPath("/stream", http::Method::POST,
                 [&streamCalled](const HttpRequest&, [[maybe_unused]] HttpResponseWriter&) { streamCalled = true; });

  auto res = router.match(http::Method::POST, "/stream");
  ASSERT_EQ(res.requestHandler, nullptr);
  ASSERT_NE(res.streamingHandler, nullptr);
  ASSERT_FALSE(res.methodNotAllowed);

  // We cannot easily construct an HttpResponseWriter here without a real HttpServer.
  // Verifying non-null streamingHandler is sufficient for the Router::match contract.
  EXPECT_FALSE(streamCalled);
}

TEST(RouterUnitTest, MethodNotAllowedAndFallback) {
  Router router;

  router.setPath("/onlyget", http::Method::GET, [](const HttpRequest&) { return HttpResponse(200); });

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

TEST(RouterUnitTest, GlobalDefaultHandlersUsedWhenNoPath) {
  Router router;

  router.setDefault([](const HttpRequest&) { return HttpResponse(204); });

  auto res = router.match(http::Method::GET, "/nope");
  ASSERT_NE(res.requestHandler, nullptr);
  ASSERT_EQ(res.streamingHandler, nullptr);
  EXPECT_FALSE(res.methodNotAllowed);

  // streaming default
  Router r2;
  bool sCalled = false;
  r2.setDefault([&sCalled](const HttpRequest&, HttpResponseWriter& writerParam) {
    sCalled = true;
    writerParam.end();
  });
  auto res2 = r2.match(http::Method::GET, "/nope");
  ASSERT_EQ(res2.requestHandler, nullptr);
  ASSERT_NE(res2.streamingHandler, nullptr);
}

TEST(RouterUnitTest, TrailingSlashRedirectAndNormalize) {
  // Redirect policy: registering /p should redirect /p/ -> AddSlash or RemoveSlash depending
  RouterConfig cfg;
  cfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect);
  Router router(cfg);

  router.setPath("/can", http::Method::GET, [](const HttpRequest&) { return HttpResponse(200); });

  // exact match
  auto resExact = router.match(http::Method::GET, "/can");
  EXPECT_NE(resExact.requestHandler, nullptr);
  EXPECT_EQ(resExact.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::None);

  // non-exact with trailing slash should request redirect (RemoveSlash)
  auto resSlashed = router.match(http::Method::GET, "/can/");
  EXPECT_EQ(resSlashed.requestHandler, nullptr);
  EXPECT_EQ(resSlashed.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::RemoveSlash);
}

TEST(RouterUnitTest, HeadFallbackToGet) {
  Router router;
  router.setPath("/hf", http::Method::GET, [](const HttpRequest&) { return HttpResponse(200); });

  // HEAD should fallback to GET handler when no explicit HEAD handler registered
  auto resHead = router.match(http::Method::HEAD, "/hf");
  EXPECT_NE(resHead.requestHandler, nullptr);
  EXPECT_EQ(resHead.streamingHandler, nullptr);
  EXPECT_FALSE(resHead.methodNotAllowed);
}

TEST(RouterUnitTest, MethodMergingAndOverwrite) {
  Router router;
  // register GET and then add POST using method-bmp OR
  router.setPath("/merge", http::Method::GET, [](const HttpRequest&) { return HttpResponse(200); });
  router.setPath("/merge", http::Method::POST, [](const HttpRequest&) { return HttpResponse(201); });

  auto rGet = router.match(http::Method::GET, "/merge");
  EXPECT_NE(rGet.requestHandler, nullptr);
  EXPECT_FALSE(rGet.methodNotAllowed);

  auto rPost = router.match(http::Method::POST, "/merge");
  EXPECT_NE(rPost.requestHandler, nullptr);
  EXPECT_FALSE(rPost.methodNotAllowed);
}

TEST(RouterUnitTest, StreamingVsNormalConflictThrows) {
  Router router;
  router.setPath("/conf", http::Method::GET, [](const HttpRequest&) { return HttpResponse(200); });
  // Attempting to register a streaming handler for the same path+method should throw
  EXPECT_THROW(router.setPath(std::string{"/conf"}, http::Method::GET,
                              Router::StreamingHandler([](const HttpRequest&, HttpResponseWriter&) {})),
               aeronet::exception);
}

TEST(RouterUnitTest, TrailingSlashStrictAndNormalize) {
  // Strict: /a/ registered does not match /a
  RouterConfig cfgStrict;
  cfgStrict.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Strict);
  Router rStrict(cfgStrict);
  rStrict.setPath("/s/", http::Method::GET, [](const HttpRequest&) { return HttpResponse(200); });
  auto res1 = rStrict.match(http::Method::GET, "/s/");
  EXPECT_NE(res1.requestHandler, nullptr);
  auto res1b = rStrict.match(http::Method::GET, "/s");
  EXPECT_EQ(res1b.requestHandler, nullptr);

  // Normalize: registering /n/ makes /n acceptable
  RouterConfig cfgNorm;
  cfgNorm.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Normalize);
  Router rNorm(cfgNorm);
  rNorm.setPath("/n/", http::Method::GET, [](const HttpRequest&) { return HttpResponse(200); });
  auto res2 = rNorm.match(http::Method::GET, "/n");
  EXPECT_NE(res2.requestHandler, nullptr);
}
