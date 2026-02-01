#include "aeronet/router.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/request-task.hpp"
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
#include "aeronet/websocket-endpoint.hpp"
#endif

namespace aeronet {

// (no test helpers declared here)

namespace {

HttpResponse OkHandler([[maybe_unused]] const HttpRequest &req) { return HttpResponse(http::StatusCodeOK); }
HttpResponse AcceptedHandler([[maybe_unused]] const HttpRequest &req) { return HttpResponse(http::StatusCodeAccepted); }
HttpResponse CreatedHandler([[maybe_unused]] const HttpRequest &req) { return HttpResponse(http::StatusCodeCreated); }

}  // namespace

class HttpResponseWriter;

class RouterTest : public ::testing::Test {
 protected:
  RouterConfig cfg;
  Router router;

  // Use an aligned storage because HttpRequest constructor should be kept private
  alignas(HttpRequest) std::byte httpRequestStorage[sizeof(HttpRequest)];

  const HttpRequest &dummyReq() { return *reinterpret_cast<const HttpRequest *>(&httpRequestStorage); }
};

TEST_F(RouterTest, RegisterAndMatchNormalHandler) {
  bool called = false;
  router.setPath(http::Method::GET, "/hello", [&called](const HttpRequest &) {
    called = true;
    return HttpResponse(http::StatusCodeOK);
  });

  auto res = router.match(http::Method::GET, "/hello");
  ASSERT_NE(res.requestHandler(), nullptr);
  ASSERT_EQ(res.streamingHandler(), nullptr);
  ASSERT_FALSE(res.methodNotAllowed);

  // Invoke the handler via the pointer to ensure it is callable and behaves correctly

  HttpResponse resp = (*res.requestHandler())(dummyReq());
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_TRUE(called);
}

TEST_F(RouterTest, MatchPatternSegmentLiteralMismatchReturnsFalse) {
  // Pattern with mixed literal/param/literal inside a single segment: prefix{}/suffix
  router.setPath(http::Method::GET, "/items/prefix{}/suffix", OkHandler);

  // Try to match with a segment that doesn't start with the required 'prefix'
  auto res = router.match(http::Method::GET, "/items/wrong/suffix");
  // No handler should be found because the dynamic segment literal part doesn't match
  EXPECT_EQ(res.requestHandler(), nullptr);
}

TEST_F(RouterTest, MatchPatternWithLiteralPrefixAndSuffixInSegment) {
  router.setPath(http::Method::GET, "/files/prefix{}end", OkHandler);

  auto res = router.match(http::Method::GET, "/files/prefixonly");
  EXPECT_EQ(res.requestHandler(), nullptr);

  res = router.match(http::Method::GET, "/files/prefixwithend");
  EXPECT_NE(res.requestHandler(), nullptr);
  ASSERT_EQ(res.pathParams.size(), 1U);
  EXPECT_EQ(res.pathParams[0].key, "0");
  EXPECT_EQ(res.pathParams[0].value, "with");

  res = router.match(http::Method::GET, "/files/prefixend");
  EXPECT_NE(res.requestHandler(), nullptr);
  ASSERT_EQ(res.pathParams.size(), 1U);
  EXPECT_EQ(res.pathParams[0].key, "0");
  EXPECT_EQ(res.pathParams[0].value, "");
}

TEST_F(RouterTest, MatchPatternSegmentConsecutiveParamsReturnsFalse) {
  // Pattern with consecutive params in a single segment: {}/{}
  router.setPath(http::Method::GET, "/data/{}/{}", OkHandler);

  // Try to match with any segment; should fail due to consecutive params
  auto res = router.match(http::Method::GET, "/data/anything");
  EXPECT_EQ(res.requestHandler(), nullptr);
}

TEST_F(RouterTest, ConsecutiveParamsWithoutSeparatorNotPermitted) {
  // Attempt to register a route with consecutive params in a single segment: {}{}
  EXPECT_THROW(router.setPath(http::Method::GET, "/consecutive/{}{}", OkHandler), std::invalid_argument);
}

TEST_F(RouterTest, ConflictingParameterNamingThrows) {
  // Register a route with a named parameter
  router.setPath(http::Method::GET, "/items/{id}/view", OkHandler);

  // Register the same pattern but using an unnamed parameter in the same segment -> should throw
  EXPECT_THROW(router.setPath(http::Method::GET, std::string{"/items/{}/view"}, CreatedHandler), std::logic_error);
}

TEST_F(RouterTest, RegisterAndMatchStreamingHandler) {
  bool streamCalled = false;
  router.setPath(http::Method::POST, "/stream",
                 [&streamCalled](const HttpRequest &, [[maybe_unused]] HttpResponseWriter &) { streamCalled = true; });

  auto res = router.match(http::Method::POST, "/stream");
  ASSERT_EQ(res.requestHandler(), nullptr);
  ASSERT_NE(res.streamingHandler(), nullptr);
  ASSERT_FALSE(res.methodNotAllowed);

  // We cannot easily construct an HttpResponseWriter here without a real SingleHttpServer.
  // Verifying non-null streamingHandler is sufficient for the Router::match contract.
  EXPECT_FALSE(streamCalled);
}

TEST_F(RouterTest, MethodNotAllowedAndFallback) {
  router.setPath(http::Method::GET, "/onlyget", OkHandler);

  // POST should result in methodNotAllowed
  auto resPost = router.match(http::Method::POST, "/onlyget");
  EXPECT_TRUE(resPost.methodNotAllowed);
  EXPECT_EQ(resPost.requestHandler(), nullptr);

  // GET should match
  auto resGet = router.match(http::Method::GET, "/onlyget");
  EXPECT_FALSE(resGet.methodNotAllowed);
  EXPECT_NE(resGet.requestHandler(), nullptr);

  // No path registered -> fallback to no handler (empty)
  auto resMissing = router.match(http::Method::GET, "/missing");
  EXPECT_EQ(resMissing.requestHandler(), nullptr);
  EXPECT_EQ(resMissing.streamingHandler(), nullptr);
  EXPECT_FALSE(resMissing.methodNotAllowed);
}

TEST_F(RouterTest, GlobalDefaultHandlersUsedWhenNoPath) {
  router.setDefault([](const HttpRequest &) { return HttpResponse(204); });

  auto res = router.match(http::Method::GET, "/nope");
  ASSERT_NE(res.requestHandler(), nullptr);
  ASSERT_EQ(res.streamingHandler(), nullptr);
  EXPECT_FALSE(res.methodNotAllowed);

  // streaming default
  Router r2;
  bool sCalled = false;
  r2.setDefault([&sCalled](const HttpRequest &, HttpResponseWriter &) { sCalled = true; });
  auto res2 = r2.match(http::Method::GET, "/nope");
  ASSERT_EQ(res2.requestHandler(), nullptr);
  ASSERT_NE(res2.streamingHandler(), nullptr);
}

TEST_F(RouterTest, TrailingSlashRedirectAndNormalize) {
  // Redirect policy: registering /p should redirect /p/ -> AddSlash or RemoveSlash depending

  cfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect);
  router = Router(cfg);

  router.setPath(http::Method::GET, "/can", OkHandler);

  // exact match
  auto resExact = router.match(http::Method::GET, "/can");
  EXPECT_NE(resExact.requestHandler(), nullptr);
  EXPECT_EQ(resExact.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::None);

  // non-exact with trailing slash should request redirect (RemoveSlash)
  auto resSlashed = router.match(http::Method::GET, "/can/");
  EXPECT_EQ(resSlashed.requestHandler(), nullptr);
  EXPECT_EQ(resSlashed.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::RemoveSlash);
}

TEST_F(RouterTest, HeadFallbackToGet) {
  router.setPath(http::Method::GET, "/hf", OkHandler);

  // HEAD should fallback to GET handler when no explicit HEAD handler registered
  auto resHead = router.match(http::Method::HEAD, "/hf");
  EXPECT_NE(resHead.requestHandler(), nullptr);
  EXPECT_EQ(resHead.streamingHandler(), nullptr);
  EXPECT_FALSE(resHead.methodNotAllowed);
}

TEST_F(RouterTest, ExplicitHeadHandlerUsed) {
  // Explicit HEAD handler should be preferred over GET
  router.setPath(http::Method::GET, "/head", OkHandler);
  router.setPath(http::Method::HEAD, "/head", CreatedHandler);

  auto res = router.match(http::Method::HEAD, "/head");
  ASSERT_NE(res.requestHandler(), nullptr);
  alignas(HttpRequest) std::byte storage[sizeof(HttpRequest)];
  const HttpRequest &req = *reinterpret_cast<const HttpRequest *>(&storage);
  EXPECT_EQ((*res.requestHandler())(req).status(), 201);
}

TEST_F(RouterTest, HeadFallbackToStreamingGet) {
  // If GET is registered as a streaming handler, HEAD should fallback to that streaming handler
  router.setPath(http::Method::GET, "/hstream", StreamingHandler([](const HttpRequest &, HttpResponseWriter &) {}));

  auto res = router.match(http::Method::HEAD, "/hstream");
  EXPECT_EQ(res.requestHandler(), nullptr);
  EXPECT_NE(res.streamingHandler(), nullptr);
  EXPECT_FALSE(res.methodNotAllowed);
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
TEST_F(RouterTest, HeadFallbackToAsyncGet) {
  // If GET is registered as an async handler, HEAD should fallback to that async handler
  router.setPath(http::Method::GET, "/haasync",
                 AsyncRequestHandler([]([[maybe_unused]] HttpRequest &req) -> RequestTask<HttpResponse> {
                   co_return HttpResponse(200);
                 }));

  auto res = router.match(http::Method::HEAD, "/haasync");
  EXPECT_EQ(res.requestHandler(), nullptr);
  EXPECT_EQ(res.streamingHandler(), nullptr);
  EXPECT_NE(res.asyncRequestHandler(), nullptr);
  EXPECT_FALSE(res.methodNotAllowed);
}
#endif

TEST_F(RouterTest, ExplicitHeadStreamingAndAsyncHandlers) {
  // Explicit streaming HEAD handler
  Router r1;
  r1.setPath(http::Method::HEAD, "/hds", StreamingHandler([](const HttpRequest &, HttpResponseWriter &) {}));
  auto r1res = r1.match(http::Method::HEAD, "/hds");
  EXPECT_EQ(r1res.requestHandler(), nullptr);
  EXPECT_NE(r1res.streamingHandler(), nullptr);

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Explicit async HEAD handler
  Router r2;
  r2.setPath(http::Method::HEAD, "/hda",
             AsyncRequestHandler(
                 []([[maybe_unused]] HttpRequest &req) -> RequestTask<HttpResponse> { co_return HttpResponse(202); }));
  auto r2res = r2.match(http::Method::HEAD, "/hda");
  EXPECT_EQ(r2res.requestHandler(), nullptr);
  EXPECT_EQ(r2res.streamingHandler(), nullptr);
  EXPECT_NE(r2res.asyncRequestHandler(), nullptr);
#endif
}

TEST_F(RouterTest, HeadMethodNotAllowedWhenNoGetOrHead) {
  router.setPath(http::Method::POST, "/onlypost", OkHandler);
  auto res = router.match(http::Method::HEAD, "/onlypost");
  EXPECT_TRUE(res.methodNotAllowed);
}

TEST_F(RouterTest, MethodMergingAndOverwrite) {
  // register GET and then add POST using method-bmp OR
  router.setPath(http::Method::GET, "/merge", OkHandler);
  router.setPath(http::Method::POST, "/merge", CreatedHandler);

  auto rGet = router.match(http::Method::GET, "/merge");
  EXPECT_NE(rGet.requestHandler(), nullptr);
  EXPECT_FALSE(rGet.methodNotAllowed);

  auto rPost = router.match(http::Method::POST, "/merge");
  EXPECT_NE(rPost.requestHandler(), nullptr);
  EXPECT_FALSE(rPost.methodNotAllowed);
}

TEST_F(RouterTest, MethodBitmapRegistersMultipleHandlers) {
  router.setPath(http::Method::GET | http::Method::POST, "/combo",
                 [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });

  auto getRes = router.match(http::Method::GET, "/combo");
  EXPECT_NE(getRes.requestHandler(), nullptr);
  EXPECT_FALSE(getRes.methodNotAllowed);

  auto postRes = router.match(http::Method::POST, "/combo");
  EXPECT_NE(postRes.requestHandler(), nullptr);
  EXPECT_FALSE(postRes.methodNotAllowed);
}

TEST_F(RouterTest, StreamingVsNormalConflictThrows) {
  router.setPath(http::Method::GET, "/conf", [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });
  // Attempting to register a streaming handler for the same path+method should throw
  EXPECT_THROW(router.setPath(http::Method::GET, std::string{"/conf"},
                              StreamingHandler([](const HttpRequest &, HttpResponseWriter &) {})),
               std::logic_error);
}

TEST_F(RouterTest, TrailingSlashStrictAndNormalize) {
  // Strict: /a/ registered does not match /a
  RouterConfig cfgStrict;
  cfgStrict.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Strict);
  Router rStrict(cfgStrict);
  rStrict.setPath(http::Method::GET, "/s/", [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });
  auto res1 = rStrict.match(http::Method::GET, "/s/");
  EXPECT_NE(res1.requestHandler(), nullptr);
  auto res1b = rStrict.match(http::Method::GET, "/s");
  EXPECT_EQ(res1b.requestHandler(), nullptr);

  // Normalize: registering /n/ makes /n acceptable
  RouterConfig cfgNorm;
  cfgNorm.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Normalize);
  Router rNorm(cfgNorm);
  rNorm.setPath(http::Method::GET, "/n/", [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });
  auto res2 = rNorm.match(http::Method::GET, "/n");
  EXPECT_NE(res2.requestHandler(), nullptr);
}

TEST_F(RouterTest, NormalizeWithWildcard) {
  router = Router(RouterConfig{}.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Normalize));

  router.setPath(http::Method::GET, "/a/*", OkHandler);

  auto res = router.match(http::Method::GET, "/a/");
  ASSERT_NE(res.requestHandler(), nullptr);

  auto res2 = router.match(http::Method::GET, "/a");
  ASSERT_NE(res2.requestHandler(), nullptr);
}

TEST_F(RouterTest, CapturesNamedParameters) {
  router.setPath(http::Method::GET, "/users/{userId}/posts/{postId}",
                 [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });

  auto res = router.match(http::Method::GET, "/users/42/posts/abc");
  ASSERT_NE(res.requestHandler(), nullptr);
  ASSERT_EQ(res.pathParams.size(), 2U);
  EXPECT_EQ(res.pathParams[0].key, "userId");
  EXPECT_EQ(res.pathParams[0].value, "42");
  EXPECT_EQ(res.pathParams[1].key, "postId");
  EXPECT_EQ(res.pathParams[1].value, "abc");
}

TEST_F(RouterTest, CapturesUnnamedParametersAsIndices) {
  router.setPath(http::Method::GET, "/files/{}/chunk/{}",
                 [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });

  auto res = router.match(http::Method::GET, "/files/alpha/chunk/123");
  ASSERT_NE(res.requestHandler(), nullptr);
  ASSERT_EQ(res.pathParams.size(), 2U);
  EXPECT_EQ(res.pathParams[0].key, "0");
  EXPECT_EQ(res.pathParams[0].value, "alpha");
  EXPECT_EQ(res.pathParams[1].key, "1");
  EXPECT_EQ(res.pathParams[1].value, "123");
}

TEST_F(RouterTest, SupportsLiteralAndParamMixWithinSegment) {
  router.setPath(http::Method::GET, "/api/v{}/foo{}bar",
                 [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });

  auto res = router.match(http::Method::GET, "/api/v1/foo123bar");
  ASSERT_NE(res.requestHandler(), nullptr);
  ASSERT_EQ(res.pathParams.size(), 2U);
  EXPECT_EQ(res.pathParams[0].value, "1");
  EXPECT_EQ(res.pathParams[1].value, "123");
}

TEST_F(RouterTest, EmptyPathInvalid) {
  EXPECT_THROW(router.setPath(http::Method::GET, "", OkHandler), std::invalid_argument);
  EXPECT_THROW((void)router.match(http::Method::GET, ""), std::invalid_argument);
  EXPECT_THROW((void)router.allowedMethods(""), std::invalid_argument);
}

TEST_F(RouterTest, PathNotStartingWithSlashInvalid) {
  EXPECT_THROW(router.setPath(http::Method::GET, "noslash", OkHandler), std::invalid_argument);
}

TEST_F(RouterTest, WildcardMatchesRemainingSegments) {
  router.setPath(http::Method::GET, "/static/*", [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });

  auto res = router.match(http::Method::GET, "/static/css/app/main.css");
  ASSERT_NE(res.requestHandler(), nullptr);
  EXPECT_EQ(res.pathParams.size(), 0U);
}

TEST_F(RouterTest, SpecialOperations) {
  router.setPath(http::Method::GET, "/x", OkHandler);
  Router moved(std::move(router));
  Router anotherRouter;
  anotherRouter = std::move(moved);

  EXPECT_TRUE(anotherRouter.match(http::Method::GET, "/x").hasHandler());

  moved = anotherRouter;
  EXPECT_TRUE(moved.match(http::Method::GET, "/x").hasHandler());

  auto &routerBis = anotherRouter;
  routerBis = anotherRouter;             // should be no-op
  routerBis = std::move(anotherRouter);  // should be no-op
}

TEST_F(RouterTest, CopyConstructorCopiesHandlersAndPatterns) {
  bool calledA = false;
  router.setPath(http::Method::GET, "/copy/a", [&calledA](const HttpRequest &) {
    calledA = true;
    return HttpResponse(200);
  });

  bool calledB = false;
  // complex pattern with params and literal mix
  router.setPath(http::Method::POST, "/files/v{}/part/{}", [&calledB](const HttpRequest &) {
    calledB = true;
    return HttpResponse(201);
  });

  // streaming handler
  bool streamCalled = false;
  router.setPath(http::Method::PUT, "/stream/x",
                 StreamingHandler([&streamCalled](const HttpRequest &, HttpResponseWriter &) { streamCalled = true; }));

  // wildcard
  router.setPath(http::Method::GET, "/wild/*", OkHandler);

  // copy-construct
  Router clone(router);

  // original handlers still work
  // prepare a dummy HttpRequest storage used for invoking handlers in tests
  alignas(HttpRequest) std::byte dummyReqStorage[sizeof(HttpRequest)];
  const HttpRequest &dummyReq = *reinterpret_cast<const HttpRequest *>(&dummyReqStorage);

  auto resFromOriginal = router.match(http::Method::GET, "/copy/a");
  ASSERT_NE(resFromOriginal.requestHandler(), nullptr);
  (*resFromOriginal.requestHandler())(dummyReq);

  auto resFromClone = clone.match(http::Method::GET, "/copy/a");
  ASSERT_NE(resFromClone.requestHandler(), nullptr);
  (*resFromClone.requestHandler())(dummyReq);

  EXPECT_TRUE(calledA);

  auto resPost = clone.match(http::Method::POST, "/files/v1/part/xyz");
  ASSERT_NE(resPost.requestHandler(), nullptr);
  (*resPost.requestHandler())(dummyReq);
  EXPECT_TRUE(calledB);

  auto resStream = clone.match(http::Method::PUT, "/stream/x");
  ASSERT_NE(resStream.streamingHandler(), nullptr);
  // don't invoke streaming handler here; presence is sufficient

  auto resWild = clone.match(http::Method::GET, "/wild/any/path/here");
  ASSERT_NE(resWild.requestHandler(), nullptr);
}

TEST_F(RouterTest, CopyAssignmentPreservesHandlersAndIsIndependent) {
  Router baseRouter;

  int invokedOriginal = 0;
  baseRouter.setPath(http::Method::GET, "/indep/x", [&invokedOriginal](const HttpRequest &) {
    ++invokedOriginal;
    return HttpResponse(200);
  });

  Router destRouter;
  destRouter = baseRouter;  // copy-assign

  // Both should match initially
  alignas(HttpRequest) std::byte dummyStorage2[sizeof(HttpRequest)];
  const HttpRequest &dummyReq2 = *reinterpret_cast<const HttpRequest *>(&dummyStorage2);

  auto rBase = baseRouter.match(http::Method::GET, "/indep/x");
  ASSERT_NE(rBase.requestHandler(), nullptr);
  (*rBase.requestHandler())(dummyReq2);
  EXPECT_EQ(invokedOriginal, 1);

  auto rDest = destRouter.match(http::Method::GET, "/indep/x");
  ASSERT_NE(rDest.requestHandler(), nullptr);
  (*rDest.requestHandler())(dummyReq2);
  // assignment should have copied handler behaviour
  EXPECT_EQ(invokedOriginal, 2);

  // Now mutate original: overwrite handler for the same path
  baseRouter.setPath(http::Method::GET, "/indep/x", CreatedHandler);

  // base now has new handler; dest should still have the old one
  auto rBase2 = baseRouter.match(http::Method::GET, "/indep/x");
  ASSERT_NE(rBase2.requestHandler(), nullptr);
  HttpResponse respBase = (*rBase2.requestHandler())(dummyReq2);
  EXPECT_EQ(respBase.status(), 201);

  auto rDest2 = destRouter.match(http::Method::GET, "/indep/x");
  ASSERT_NE(rDest2.requestHandler(), nullptr);
  HttpResponse respDest = (*rDest2.requestHandler())(dummyReq2);
  EXPECT_EQ(respDest.status(), 200);
}

TEST_F(RouterTest, CopyPreservesTrailingSlashVariantsAndMethodTypes) {
  Router rTs;
  rTs.setPath(http::Method::GET, "/ts/", OkHandler);
  rTs.setPath(http::Method::POST, "/ts", CreatedHandler);

  Router cTs(rTs);

  // GET /ts/ should match in clone
  auto rg = cTs.match(http::Method::GET, "/ts/");
  ASSERT_NE(rg.requestHandler(), nullptr);

  // POST /ts should match in clone
  auto rp = cTs.match(http::Method::POST, "/ts");
  ASSERT_NE(rp.requestHandler(), nullptr);
  alignas(HttpRequest) std::byte dummyTsReq[sizeof(HttpRequest)];
  const HttpRequest &dummyTs = *reinterpret_cast<const HttpRequest *>(&dummyTsReq);
  HttpResponse resp = (*rp.requestHandler())(dummyTs);
  EXPECT_EQ(resp.status(), 201);
}

TEST_F(RouterTest, CopyHandlesHeadFallbackAndMethodBitmaps) {
  Router rHf;
  rHf.setPath(http::Method::GET, "/hfcopy", OkHandler);
  Router cHf = rHf;

  // HEAD should fallback to GET in clone
  auto rh = cHf.match(http::Method::HEAD, "/hfcopy");
  EXPECT_NE(rh.requestHandler(), nullptr);
  EXPECT_FALSE(rh.methodNotAllowed);
}

TEST_F(RouterTest, CopyPreservesLiteralOnlyFastPath) {
  // Test that literal-only routes (no patterns) are correctly cloned with fast-path optimization
  Router original;

  int callCount = 0;
  original.setPath(http::Method::GET, "/api/v1/users/list", [&callCount](const HttpRequest &) {
    ++callCount;
    return HttpResponse(200);
  });

  // Clone the router
  Router clone = original;

  // Verify both original and clone work correctly
  alignas(HttpRequest) std::byte dummyStorage[sizeof(HttpRequest)];
  const HttpRequest &dummyReq = *reinterpret_cast<const HttpRequest *>(&dummyStorage);

  auto resOriginal = original.match(http::Method::GET, "/api/v1/users/list");
  ASSERT_NE(resOriginal.requestHandler(), nullptr);
  (*resOriginal.requestHandler())(dummyReq);
  EXPECT_EQ(callCount, 1);

  auto resClone = clone.match(http::Method::GET, "/api/v1/users/list");
  ASSERT_NE(resClone.requestHandler(), nullptr);
  (*resClone.requestHandler())(dummyReq);
  EXPECT_EQ(callCount, 2);

  // Verify independence: modifying original doesn't affect clone
  original.setPath(http::Method::GET, "/api/v1/users/list", [](const HttpRequest &) { return HttpResponse(404); });

  auto resCloneAfter = clone.match(http::Method::GET, "/api/v1/users/list");
  ASSERT_NE(resCloneAfter.requestHandler(), nullptr);
  HttpResponse resp = (*resCloneAfter.requestHandler())(dummyReq);
  EXPECT_EQ(resp.status(), 200);  // Clone still has old handler
  EXPECT_EQ(callCount, 3);
}

TEST_F(RouterTest, RegisterWildcardTwiceExercisesExistingChild) {
  // First registration should allocate pWildcardChild
  router.setPath(http::Method::GET, "/dup/*", OkHandler);

  // Second registration for the same pattern should find the existing pWildcardChild and not reallocate.
  // This exercises the branch where node->pWildcardChild != nullptr inside setPathInternal.
  router.setPath(http::Method::POST, "/dup/*", CreatedHandler);

  // Matching both GET and POST should succeed
  auto m1 = router.match(http::Method::GET, "/dup/anything/here");
  EXPECT_NE(m1.requestHandler(), nullptr);
  auto m2 = router.match(http::Method::POST, "/dup/other");
  EXPECT_NE(m2.requestHandler(), nullptr);
}

TEST_F(RouterTest, DuplicateDynamicEdge) {
  // Register a dynamic pattern with a parameter in the segment
  router.setPath(http::Method::GET, "/users/{id}/profile", OkHandler);

  // Register the same dynamic pattern again (should find existing dynamic edge)
  // This path uses std::string to exercise the overloads and code paths used in practice.
  router.setPath(http::Method::POST, "/users/{id}/profile", CreatedHandler);

  // Match to ensure router still behaves
  EXPECT_NE(router.match(http::Method::GET, "/users/42/profile").requestHandler(), nullptr);
  EXPECT_NE(router.match(http::Method::POST, "/users/42/profile").requestHandler(), nullptr);
}

TEST_F(RouterTest, NonCopyableHandlerAcrossMultipleMethods) {
  // Construct a callable type that becomes invalid when copied: copies will throw on invocation.
  struct Poisonable {
    // mutable so operator() can be const
    mutable bool valid{true};
    Poisonable() = default;
    // Copying creates an invalid copy
    Poisonable(const Poisonable & /*other*/) : valid(false) {}
    HttpResponse operator()(const HttpRequest & /*req*/) const {
      if (!valid) {
        throw std::bad_function_call();
      }
      return HttpResponse(200);
    }
  };

  std::function<HttpResponse(const HttpRequest &)> handler = Poisonable{};

  http::MethodBmp methods =
      static_cast<http::MethodBmp>(http::Method::GET) | static_cast<http::MethodBmp>(http::Method::POST);

  router.setPath(methods, "/nc", std::move(handler));

  alignas(HttpRequest) std::byte reqStorage[sizeof(HttpRequest)];
  const HttpRequest &dummyReq = *reinterpret_cast<const HttpRequest *>(&reqStorage);

  // Invoke both methods and record outcomes: one should succeed, the other should throw
  int successCount = 0;
  int throwCount = 0;

  auto tryInvoke = [&](http::Method method) {
    auto res = router.match(method, "/nc");
    if (res.requestHandler() == nullptr) {
      return;  // treat as not registered
    }
    try {
      HttpResponse response = (*res.requestHandler())(dummyReq);
      if (response.status() == 200) {
        ++successCount;
      }
    } catch (const std::bad_function_call &) {
      ++throwCount;
    }
  };

  tryInvoke(http::Method::GET);
  tryInvoke(http::Method::POST);

  // At least one method should throw std::bad_function_call due to an invalid copied callable
  EXPECT_GE(throwCount, 1);
}

TEST_F(RouterTest, CompilePatternErrorsAndEscapes) {
  // Path must begin with '/'
  EXPECT_THROW(router.setPath(http::Method::GET, "no-slash", OkHandler), std::invalid_argument);

  // Empty segment
  EXPECT_THROW(router.setPath(http::Method::GET, "/a//b", OkHandler), std::invalid_argument);

  // Unterminated brace
  EXPECT_THROW(router.setPath(http::Method::GET, "/u{bad", OkHandler), std::invalid_argument);

  // Escaped braces should be accepted literally
  router.setPath(http::Method::GET, "/literal/{{}}/end", OkHandler);
  auto res = router.match(http::Method::GET, "/literal/{} /end");
  // no match because space inserted; ensure matching doesn't crash
  EXPECT_EQ(res.requestHandler(), nullptr);

  res = router.match(http::Method::GET, "/literal/{}/end");
  EXPECT_NE(res.requestHandler(), nullptr);
}

TEST_F(RouterTest, MixedEscapedBracesAndNamedParams) {
  router.setPath(http::Method::GET, "/mix/{{}}/{id}/{{end}}", OkHandler);

  auto res = router.match(http::Method::GET, "/mix/{}/42/{end}");
  ASSERT_NE(res.requestHandler(), nullptr);
  ASSERT_EQ(res.pathParams.size(), 1U);
  EXPECT_EQ(res.pathParams[0].key, "id");
  EXPECT_EQ(res.pathParams[0].value, "42");
}

TEST_F(RouterTest, MixedEscapedBracesAndUnnamedParams) {
  router.setPath(http::Method::GET, "/mix/{{}}/{}/{{end}}", OkHandler);

  auto res = router.match(http::Method::GET, "/mix/{}/value/{end}");
  ASSERT_NE(res.requestHandler(), nullptr);
  ASSERT_EQ(res.pathParams.size(), 1U);
  EXPECT_EQ(res.pathParams[0].key, "0");
  EXPECT_EQ(res.pathParams[0].value, "value");
}

TEST_F(RouterTest, MixedNamedAndUnnamedParamsDisallowed) {
  EXPECT_THROW(router.setPath(http::Method::GET, "/mix/{}/{id}", OkHandler), std::invalid_argument);
  EXPECT_THROW(router.setPath(http::Method::GET, "/mix/{id}/{}/", OkHandler), std::invalid_argument);
}

TEST_F(RouterTest, UnterminatedBraceInPatternThrows) {
  EXPECT_THROW(router.setPath(http::Method::GET, "/bad/{param", OkHandler), std::invalid_argument);
  EXPECT_THROW(router.setPath(http::Method::GET, "/also/bad/{", OkHandler), std::invalid_argument);
}

TEST_F(RouterTest, FindWildcardEscapedAndTrailingOpenBrace) {
  // Explicit case: escaped open brace '{{' should be treated as literal and skipped by FindWildcard
  router.setPath(http::Method::GET, "/fw/{{}}/{id}/end", [](const HttpRequest &) { return HttpResponse(200); });
  auto res = router.match(http::Method::GET, "/fw/{}/42/end");
  ASSERT_NE(res.requestHandler(), nullptr);
  ASSERT_EQ(res.pathParams.size(), 1U);
  EXPECT_EQ(res.pathParams[0].key, "id");
  EXPECT_EQ(res.pathParams[0].value, "42");

  // Trailing open brace should raise an error (FindWildcard sees '{' at end -> second condition false)
  EXPECT_THROW(router.setPath(http::Method::GET, "/trailing/{", OkHandler), std::invalid_argument);
}

TEST_F(RouterTest, EscapedAsteriskNonEscapedFollowedByNonAsterisk) {
  // Single '*' followed by another character in the same segment is treated as a literal
  EXPECT_NO_THROW(router.setPath(http::Method::GET, "/case/*x/end", OkHandler));
  auto r1 = router.match(http::Method::GET, "/case/*x/end");
  ASSERT_NE(r1.requestHandler(), nullptr);

  // Single '*' alone in a non-terminal segment is now treated as a literal
  EXPECT_NO_THROW(router.setPath(http::Method::GET, "/case/*/end", OkHandler));
  auto r2 = router.match(http::Method::GET, "/case/*/end");
  ASSERT_NE(r2.requestHandler(), nullptr);

  // Terminal single '*' (alone in its segment) is a wildcard
  EXPECT_NO_THROW(router.setPath(http::Method::GET, "/case/*", OkHandler));
  auto res = router.match(http::Method::GET, "/case/anything/here");
  ASSERT_NE(res.requestHandler(), nullptr);
}

TEST_F(RouterTest, DirectFindWildcardEdgeCases) {
  // Trailing open brace should be rejected by public API
  EXPECT_THROW(router.setPath(http::Method::GET, "/trailing/{", OkHandler), std::invalid_argument);

  // Single '*' alone in a non-terminal segment is now treated as literal (not an error)
  EXPECT_NO_THROW(router.setPath(http::Method::GET, "/case/*/end", OkHandler));
  auto r0 = router.match(http::Method::GET, "/case/*/end");
  EXPECT_NE(r0.requestHandler(), nullptr);

  // Asterisk inside a segment (mixed with other chars) is treated as a literal
  EXPECT_NO_THROW(router.setPath(http::Method::GET, "/case/*x/end", OkHandler));
  auto r1 = router.match(http::Method::GET, "/case/*x/end");
  EXPECT_NE(r1.requestHandler(), nullptr);

  // Escaped close-brace inside parameter: register and match
  EXPECT_NO_THROW(router.setPath(http::Method::GET, "/x/{a}}}/end", OkHandler));
  auto r2 = router.match(http::Method::GET, "/x/a}/end");
  EXPECT_NE(r2.requestHandler(), nullptr);

  // Nested open brace inside parameter should be rejected
  EXPECT_THROW(router.setPath(http::Method::GET, "/x/{a{b}c}/end", OkHandler), std::invalid_argument);
}

TEST_F(RouterTest, AsteriskInsideSegmentTreatedAsLiteral) {
  // A single '*' inside a segment (not at the end) must be treated as a literal character
  EXPECT_NO_THROW(router.setPath(http::Method::GET, "/lit/pa*rt/end/*", OkHandler));
  auto res = router.match(http::Method::GET, "/lit/pa*rt/end/toto/tata");
  ASSERT_NE(res.requestHandler(), nullptr);
}

TEST_F(RouterTest, OnlyLastAsteriskIsWildcard) {
  router.setPath(http::Method::GET, "/double/*/*", OkHandler);
  auto res = router.match(http::Method::GET, "/double/*/thing");
  ASSERT_NE(res.requestHandler(), nullptr);
}

TEST_F(RouterTest, WildcardConflictsWithExistingWildcard) {
  // Register a true wildcard
  router.setPath(http::Method::GET, "/a/*", OkHandler);

  // Attempting to add another wildcard at the same position is allowed
  // (it will overwrite with a warning logged)
  router.setPath(http::Method::GET, "/a/*", AcceptedHandler);

  // Verify that it was overwritten
  auto res = router.match(http::Method::GET, "/a/anything");
  ASSERT_NE(res.requestHandler(), nullptr);
  HttpResponse resp = (*res.requestHandler())(dummyReq());
  EXPECT_EQ(resp.status(), 202);
}

TEST_F(RouterTest, WildcardWithPathWithTrailingSlash) {
  for (auto policy : {RouterConfig::TrailingSlashPolicy::Strict, RouterConfig::TrailingSlashPolicy::Normalize,
                      RouterConfig::TrailingSlashPolicy::Redirect}) {
    router = Router(RouterConfig{}.withTrailingSlashPolicy(policy));
    router.setPath(http::Method::GET, "/files/*/something", CreatedHandler);
    router.setPath(http::Method::GET, "/files/*", OkHandler);
    router.setPath(http::Method::GET, "/files/", AcceptedHandler);
    auto res = router.match(http::Method::GET, "/files/path/to/resource.txt");
    ASSERT_NE(res.requestHandler(), nullptr);
    EXPECT_EQ((*res.requestHandler())(dummyReq()).status(), 200);

    auto res2 = router.match(http::Method::GET, "/files/");
    ASSERT_NE(res2.requestHandler(), nullptr);
    EXPECT_EQ((*res2.requestHandler())(dummyReq()).status(), 202);

    auto res3 = router.match(http::Method::GET, "/files/*/something");
    ASSERT_NE(res3.requestHandler(), nullptr);
    EXPECT_EQ((*res3.requestHandler())(dummyReq()).status(), 201);
  }
}

TEST_F(RouterTest, DoubleAsteriskAtTheEndIsNotAWildcard) {
  router.setPath(http::Method::GET, "/end/**", OkHandler);
  auto res1 = router.match(http::Method::GET, "/end/*anything/here");
  EXPECT_EQ(res1.requestHandler(), nullptr);

  auto res2 = router.match(http::Method::GET, "/end/*");
  EXPECT_EQ(res2.requestHandler(), nullptr);

  auto res3 = router.match(http::Method::GET, "/end");
  EXPECT_EQ(res3.requestHandler(), nullptr);

  auto res4 = router.match(http::Method::GET, "/end/**");
  EXPECT_NE(res4.requestHandler(), nullptr);
}

TEST_F(RouterTest, AsteriskPartOfLastSegmentIsNotWildcard) {
  router.setPath(http::Method::GET, "/segment/part**", OkHandler);
  auto res1 = router.match(http::Method::GET, "/segment/part*anything");
  EXPECT_EQ(res1.requestHandler(), nullptr);

  auto res2 = router.match(http::Method::GET, "/segment/part**");
  EXPECT_NE(res2.requestHandler(), nullptr);
}

TEST_F(RouterTest, AsteriskWithPatternIsNotWildcard) {
  router.setPath(http::Method::PUT, "/api/**/{id}/data", OkHandler);
  auto res = router.match(http::Method::PUT, "/api/**/part*anything/data");
  EXPECT_NE(res.requestHandler(), nullptr);
}

TEST_F(RouterTest, AsteriskPartOfPatternName) {
  // Double asterisks with param in a pattern route
  router.setPath(http::Method::GET, "/pattern/{pa**rt}/end", OkHandler);
  auto res = router.match(http::Method::GET, "/pattern/pa*rt/end");
  EXPECT_NE(res.requestHandler(), nullptr);
  ASSERT_EQ(res.pathParams.size(), 1U);
  EXPECT_EQ(res.pathParams[0].key, "pa**rt");
  EXPECT_EQ(res.pathParams[0].value, "pa*rt");
}

TEST_F(RouterTest, ParamNameIsAsteriskOnly) {
  // Double asterisks with param in a pattern route
  router.setPath(http::Method::GET, "/pattern/{*}/end", OkHandler);
  auto res = router.match(http::Method::GET, "/pattern/salut/end");
  EXPECT_NE(res.requestHandler(), nullptr);
  ASSERT_EQ(res.pathParams.size(), 1U);
  EXPECT_EQ(res.pathParams[0].key, "*");
  EXPECT_EQ(res.pathParams[0].value, "salut");
}

TEST_F(RouterTest, WildcardConflictAndTerminalRules) {
  // Single * in non-terminal segment is now accepted as literal
  EXPECT_NO_THROW(router.setPath(http::Method::GET, "/bad/*/here", OkHandler));

  // wildcard matching precedence and allowedMethods
  router.setPath(http::Method::GET, "/files/*", OkHandler);
  router.setPath(http::Method::POST, "/files/upload", CreatedHandler);

  auto gm = router.match(http::Method::GET, "/files/foo/bar");
  EXPECT_NE(gm.requestHandler(), nullptr);

  auto pm = router.match(http::Method::POST, "/files/upload");
  EXPECT_NE(pm.requestHandler(), nullptr);
}

TEST_F(RouterTest, AsteriskAllowedInParamName) {
  // Parameter names may include '*' characters and should not be treated as wildcards
  EXPECT_NO_THROW(router.setPath(http::Method::GET, "/items/{id*}/detail", OkHandler));
  auto res = router.match(http::Method::GET, "/items/xyz/detail");
  ASSERT_NE(res.requestHandler(), nullptr);
  ASSERT_EQ(res.pathParams.size(), 1U);
  EXPECT_EQ(res.pathParams[0].key, "id*");
  EXPECT_EQ(res.pathParams[0].value, "xyz");
}

TEST_F(RouterTest, AllowedMethodsAndGlobalFallback) {
  router.setDefault([](const HttpRequest &) { return HttpResponse(204); });
  router.setPath(http::Method::GET | http::Method::POST, "/combo2", OkHandler);

  auto allowed = router.allowedMethods("/combo2");
  EXPECT_TRUE(http::IsMethodIdxSet(allowed, MethodToIdx(http::Method::GET)));
  EXPECT_TRUE(http::IsMethodIdxSet(allowed, MethodToIdx(http::Method::POST)));

  // Path not registered -> all methods allowed because global handler present
  auto allAllowed = router.allowedMethods("/nope");
  EXPECT_NE(allAllowed, 0U);
}

TEST_F(RouterTest, AllowedMethodsNoMatch) {
  // Path not registered -> all methods allowed because async global handler present
  auto allAllowed = router.allowedMethods("/still-missing");
  // All-methods bitmap should be non-zero and contain at least GET and POST bits
  EXPECT_EQ(allAllowed, 0U);
  EXPECT_FALSE(http::IsMethodIdxSet(allAllowed, MethodToIdx(http::Method::GET)));
  EXPECT_FALSE(http::IsMethodIdxSet(allAllowed, MethodToIdx(http::Method::POST)));
}

TEST_F(RouterTest, InvalidTrailingSlashPolicyNeverMatches) {
  cfg.withTrailingSlashPolicy(static_cast<RouterConfig::TrailingSlashPolicy>(-1));
  router = Router(cfg);
  router.setPath(http::Method::GET, "/test", OkHandler);

  auto res = router.match(http::Method::GET, "/test");
  EXPECT_EQ(res.requestHandler(), nullptr);
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
TEST_F(RouterTest, AllowedMethodsGlobalAsyncFallback) {
  // Install an async global handler and ensure allowedMethods returns all methods
  router.setDefault(AsyncRequestHandler(
      []([[maybe_unused]] HttpRequest &req) -> RequestTask<HttpResponse> { co_return HttpResponse(204); }));

  // Path not registered -> all methods allowed because async global handler present
  auto allAllowed = router.allowedMethods("/still-missing");
  // All-methods bitmap should be non-zero and contain at least GET and POST bits
  EXPECT_NE(allAllowed, 0U);
  EXPECT_TRUE(http::IsMethodIdxSet(allAllowed, MethodToIdx(http::Method::GET)));
  EXPECT_TRUE(http::IsMethodIdxSet(allAllowed, MethodToIdx(http::Method::POST)));
}
#endif

TEST_F(RouterTest, TrailingSlashEdgeCases) {
  // Normalize should accept both
  cfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Normalize);
  router = Router(cfg);
  router.setPath(http::Method::GET, "/norm/", OkHandler);
  auto resNorm = router.match(http::Method::GET, "/norm");
  EXPECT_NE(resNorm.requestHandler(), nullptr);

  // Strict must differentiate
  RouterConfig cs;
  cs.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Strict);
  Router rs(cs);
  rs.setPath(http::Method::GET, "/str/", OkHandler);
  auto resStrict = rs.match(http::Method::GET, "/str");
  EXPECT_EQ(resStrict.requestHandler(), nullptr);
}

TEST_F(RouterTest, ConflictingWildcardAndExact) {
  router.setPath(http::Method::GET, "/a/b", OkHandler);
  router.setPath(http::Method::GET, "/a/*", OkHandler);

  // exact should win
  auto ex = router.match(http::Method::GET, "/a/b");
  EXPECT_NE(ex.requestHandler(), nullptr);
}

// Fixture to test behavior across different trailing-slash policies.
class RouterTestTrailingPolicy : public ::testing::Test {
 protected:
  // Helper to build a router with the given policy and register a pair of routes:
  // - one registered without trailing slash ("/tp")
  // - one registered with trailing slash ("/tp/")
  // and return the router by value.
  static Router makeRouter(RouterConfig::TrailingSlashPolicy policy) {
    RouterConfig cfg;
    cfg.withTrailingSlashPolicy(policy);
    Router router(cfg);
    router.setPath(http::Method::GET, "/tp", OkHandler);
    router.setPath(http::Method::GET, "/tp/", AcceptedHandler);

    router.setPath(http::Method::GET, "/tp/{bar}", OkHandler);
    router.setPath(http::Method::GET, "/tp/{bar}/", AcceptedHandler);

    return router;
  }
};

TEST_F(RouterTestTrailingPolicy, NormalizeAcceptsBothForms) {
  Router router = RouterTestTrailingPolicy::makeRouter(RouterConfig::TrailingSlashPolicy::Normalize);

  // both forms should match a handler (prefer the exact-registered variant)
  auto resNoSlash = router.match(http::Method::GET, "/tp");
  EXPECT_NE(resNoSlash.requestHandler(), nullptr);
  EXPECT_EQ(resNoSlash.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::None);

  auto resWithSlash = router.match(http::Method::GET, "/tp/");
  EXPECT_NE(resWithSlash.requestHandler(), nullptr);
  EXPECT_EQ(resWithSlash.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::None);
}

TEST_F(RouterTestTrailingPolicy, StrictDistinguishesForms) {
  Router router = RouterTestTrailingPolicy::makeRouter(RouterConfig::TrailingSlashPolicy::Strict);

  // strict should only match the exact variant
  auto resNoSlash = router.match(http::Method::GET, "/tp");
  EXPECT_NE(resNoSlash.requestHandler(), nullptr);

  auto resWithSlash = router.match(http::Method::GET, "/tp/");
  EXPECT_NE(resWithSlash.requestHandler(), nullptr);
  // ensure that matching the opposite form does not return the other's handler implicitly
  // The router should not redirect in Strict mode; instead both registered variants coexist
  EXPECT_EQ(resWithSlash.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::None);

  // test with patterns as well
  resNoSlash = router.match(http::Method::GET, "/tp/123");
  EXPECT_NE(resNoSlash.requestHandler(), nullptr);

  resWithSlash = router.match(http::Method::GET, "/tp/123/");
  EXPECT_NE(resWithSlash.requestHandler(), nullptr);

  EXPECT_EQ(resWithSlash.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::None);
}

TEST_F(RouterTestTrailingPolicy, RedirectRequestsRedirect1) {
  Router router = RouterTestTrailingPolicy::makeRouter(RouterConfig::TrailingSlashPolicy::Redirect);

  // When Redirect policy is active, requesting the non-registered variant should produce a redirect indicator
  // Since we registered both forms, invoking the opposite form should trigger the appropriate redirect behaviour
  auto resNoSlash = router.match(http::Method::GET, "/tp");
  EXPECT_NE(resNoSlash.requestHandler(), nullptr);

  auto resWithSlash = router.match(http::Method::GET, "/tp/");
  EXPECT_NE(resWithSlash.requestHandler(), nullptr);

  resNoSlash = router.match(http::Method::GET, "/tp/123");
  EXPECT_NE(resNoSlash.requestHandler(), nullptr);

  resWithSlash = router.match(http::Method::GET, "/tp/123/");
  EXPECT_NE(resWithSlash.requestHandler(), nullptr);
}

TEST_F(RouterTestTrailingPolicy, RedirectRequestsRedirect2) {
  // Now check cross-requests: if we temporarily create a router that only has the no-slash registered,
  // Redirect policy should request AddSlash when matching "/tp/" and RemoveSlash when matching "/tp" if vice-versa.
  Router router(RouterConfig().withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect));

  router.setPath(http::Method::GET, "/onlynoslash", OkHandler);

  auto rSlashed = router.match(http::Method::GET, "/onlynoslash/");
  EXPECT_EQ(rSlashed.requestHandler(), nullptr);
  EXPECT_EQ(rSlashed.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::RemoveSlash);

  router.setPath(http::Method::GET, "/onlynoslash/{bar}", OkHandler);

  rSlashed = router.match(http::Method::GET, "/onlynoslash/123/");
  EXPECT_EQ(rSlashed.requestHandler(), nullptr);
  EXPECT_EQ(rSlashed.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::RemoveSlash);
}

TEST_F(RouterTestTrailingPolicy, RedirectRequestsRedirect3) {
  Router router(RouterConfig().withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect));
  router.setPath(http::Method::GET, "/onlywithslash/", OkHandler);
  auto rNoSlash = router.match(http::Method::GET, "/onlywithslash");
  EXPECT_EQ(rNoSlash.requestHandler(), nullptr);
  EXPECT_EQ(rNoSlash.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::AddSlash);

  router.setPath(http::Method::GET, "/onlywithslash/{bar}/", OkHandler);
  rNoSlash = router.match(http::Method::GET, "/onlywithslash/123");
  EXPECT_EQ(rNoSlash.requestHandler(), nullptr);
  EXPECT_EQ(rNoSlash.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::AddSlash);
}

// The large stress scenario is covered by `LargeNumberOfPatternsAndSegments_WithTrailingPolicies` which
// runs the same registration/match logic across different trailing-slash policies. The older
// `LargeNumberOfPatternsAndSegments` test has been removed to avoid duplication.

// Run the large stress registration+match scenario for each trailing-slash policy to
// ensure the router behaves correctly under different normalization/redirect semantics.
TEST_F(RouterTest, LargeNumberOfPatternsAndSegments_WithTrailingPolicies) {
  const std::array<RouterConfig::TrailingSlashPolicy, 3> policies = {
      RouterConfig::TrailingSlashPolicy::Normalize,
      RouterConfig::TrailingSlashPolicy::Strict,
      RouterConfig::TrailingSlashPolicy::Redirect,
  };

  for (auto policy : policies) {
    cfg.withTrailingSlashPolicy(policy);
    router = Router(cfg);

    const uint32_t routeCount = 1500;
    const std::size_t segments = 8;

    vector<int> called(routeCount, 0);

    // registration lambda so we reuse logic
    auto registerRoutes = [&](Router &router) {
      for (uint32_t idx = 0; idx < routeCount; ++idx) {
        http::MethodBmp registerMethod{};
        switch (idx % 4) {
          case 0:
            registerMethod = static_cast<http::MethodBmp>(http::Method::GET);
            break;
          case 1:
            registerMethod = static_cast<http::MethodBmp>(http::Method::POST);
            break;
          case 2:
            registerMethod = static_cast<http::MethodBmp>(http::Method::PUT);
            break;
          default:
            registerMethod = http::Method::GET | http::Method::POST;
            break;
        }

        std::string path = "/r/tp/id" + std::to_string(idx);
        for (std::size_t segIdx = 0; segIdx < segments; ++segIdx) {
          if (segIdx % 2 == 0) {
            path += "/seg" + std::to_string(segIdx);
          } else {
            path += "/{}";
          }
        }

        // Mix trailing slash registration depending on index to exercise both variants
        bool withTrailingSlash = (idx % 7 == 0);
        if (withTrailingSlash && path.back() != '/') {
          path += '/';
        }

        router.setPath(registerMethod, path, [idx, &called](const HttpRequest &) {
          called[idx]++;
          return HttpResponse(200);
        });
      }
    };

    // matching lambda
    auto matchAndInvoke = [&](Router &router) {
      alignas(HttpRequest) std::byte dummyStorage[sizeof(HttpRequest)];
      const HttpRequest &dummyReq = *reinterpret_cast<const HttpRequest *>(&dummyStorage);

      for (uint32_t idx = 0; idx < routeCount; ++idx) {
        std::string matchPath = "/r/tp/id" + std::to_string(idx);
        for (std::size_t segIdx = 0; segIdx < segments; ++segIdx) {
          if (segIdx % 2 == 0) {
            matchPath += "/seg" + std::to_string(segIdx);
          } else {
            matchPath += "/val" + std::to_string(idx);
          }
        }

        http::Method matchMethod;
        switch (idx % 4) {
          case 0:
            matchMethod = http::Method::GET;
            break;
          case 1:
            matchMethod = http::Method::POST;
            break;
          case 2:
            matchMethod = http::Method::PUT;
            break;
          default:
            matchMethod = http::Method::GET;
            break;
        }

        // When policy is Strict or Normalize, router may accept or reject the slashed form.
        // We registered some routes with a trailing slash; match the same form we registered.
        if ((idx % 7) == 0 && matchPath.back() != '/') {
          matchPath += '/';
        }

        auto res = router.match(matchMethod, matchPath);
        ASSERT_NE(res.requestHandler(), nullptr)
            << "No handler for path: " << matchPath << " policy=" << static_cast<int>(policy);
        (*res.requestHandler())(dummyReq);
      }
    };

    // perform registration and matching
    registerRoutes(router);
    matchAndInvoke(router);

    // verify
    for (uint32_t idx = 0; idx < routeCount; ++idx) {
      EXPECT_EQ(called[idx], 1) << "Handler not invoked for index " << idx << " policy=" << static_cast<int>(policy);
    }
  }
}

#ifdef AERONET_ENABLE_WEBSOCKET
TEST_F(RouterTest, RegisterAndMatchWebSocketEndpoint) {
  WebSocketEndpoint wsEndpoint;

  wsEndpoint.config.maxMessageSize = 1024;

  // Register a WebSocket endpoint
  router.setWebSocket("/ws", std::move(wsEndpoint));
  router.setWebSocket("/path-with-trailing-slash/", WebSocketEndpoint{});

  // Match with GET should succeed and have the endpoint
  auto resGet = router.match(http::Method::GET, "/ws");
  EXPECT_NE(resGet.pWebSocketEndpoint, nullptr);
  EXPECT_FALSE(resGet.methodNotAllowed);

  // Match with POST should not have the endpoint
  auto resPost = router.match(http::Method::POST, "/ws");
  EXPECT_NE(resPost.pWebSocketEndpoint, nullptr);  // endpoint is still exposed
  EXPECT_TRUE(resPost.methodNotAllowed);           // but method is not allowed

  // Match with trailing slash should succeed
  auto resSlash = router.match(http::Method::GET, "/path-with-trailing-slash/");
  EXPECT_NE(resSlash.pWebSocketEndpoint, nullptr);
  EXPECT_FALSE(resSlash.methodNotAllowed);
}
#endif

TEST_F(RouterTest, MatchesWildcardTerminalSegment) {
  // Register a wildcard terminal route /files/*
  router.setPath(http::Method::GET, "/files/*", [](const HttpRequest &) { return HttpResponse{}; });
  router.setPath(http::Method::GET, "/files/*", [](const HttpRequest &) { return HttpResponse{}; });

  // Matching /files/anything/else should match the wildcard route
  auto res = router.match(http::Method::GET, "/files/some/deep/path");
  EXPECT_TRUE(res.hasHandler());
}

TEST_F(RouterTest, IsWildcardStartAsterisk_StaticBeforeCatchAll) {
  // Register a static child under /star so subsequent insertion leaves remaining path '*'
  router.setPath(http::Method::GET, "/star/x", OkHandler);

  // Now insert catch-all; insertion should see '*' as the next char
  router.setPath(http::Method::POST, "/star/*", OkHandler);

  EXPECT_FALSE(router.match(http::Method::GET, "/star/foo/bar").hasHandler());
  EXPECT_TRUE(router.match(http::Method::GET, "/star/x").hasHandler());
  EXPECT_TRUE(router.match(http::Method::POST, "/star/foo/bar").hasHandler());
}

TEST_F(RouterTest, WildcardStrictTrailingSlashBehavior) {
  router = Router(RouterConfig{}.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Strict));

  // Register wildcard terminal route without trailing slash
  router.setPath(http::Method::GET, "/wild/*", OkHandler);
  // /wild/*/ is accepted as a literal * with trailing slash (not a wildcard with slash)
  router.setPath(http::Method::GET, "/wild/*/", AcceptedHandler);

  // Should match non-slashed request (wildcard terminal segment)
  auto noSlash = router.match(http::Method::GET, "/wild/one/two");
  ASSERT_NE(noSlash.requestHandler(), nullptr);
  EXPECT_EQ((*noSlash.requestHandler())(dummyReq()).status(), 200);

  // Should NOT match a request where the path has an extra trailing slash at the end
  auto withSlash = router.match(http::Method::GET, "/wild/one/two/");
  EXPECT_EQ(withSlash.requestHandler(), nullptr);

  auto exact = router.match(http::Method::GET, "/wild/*/");
  ASSERT_NE(exact.requestHandler(), nullptr);
  EXPECT_EQ((*exact.requestHandler())(dummyReq()).status(), 202);
}

TEST_F(RouterTest, WildcardNormalizeOrRedirectTrailingSlashBehavior) {
  for (auto policy : {RouterConfig::TrailingSlashPolicy::Normalize, RouterConfig::TrailingSlashPolicy::Redirect}) {
    router = Router(RouterConfig{}.withTrailingSlashPolicy(policy));

    // Register wildcard terminal route without trailing slash
    router.setPath(http::Method::GET, "/wild/*", OkHandler);
    // In normalize mode, the registered path should override the previous.
    router.setPath(http::Method::GET, "/wild/*/", AcceptedHandler);

    // Should match non-slashed request (wildcard terminal segment)
    auto noSlash = router.match(http::Method::GET, "/wild/one/two");
    ASSERT_NE(noSlash.requestHandler(), nullptr);
    EXPECT_EQ((*noSlash.requestHandler())(dummyReq()).status(), 202);

    auto withSlash = router.match(http::Method::GET, "/wild/one/two/");
    ASSERT_NE(withSlash.requestHandler(), nullptr);
    EXPECT_EQ((*withSlash.requestHandler())(dummyReq()).status(), 202);

    auto exact = router.match(http::Method::GET, "/wild/*/");
    ASSERT_NE(exact.requestHandler(), nullptr);
    EXPECT_EQ((*exact.requestHandler())(dummyReq()).status(), 202);
  }
}

TEST_F(RouterTest, PatternStringRootAndComplexPattern) {
  // Trigger patternString for the root path by overwriting the handler (invokes logging that calls patternString())
  router.setPath(http::Method::GET, "/", OkHandler);
  // Overwrite to force the logging path that calls patternString()
  router.setPath(http::Method::GET, std::string{"/"}, CreatedHandler);

  // Complex pattern with literal, unnamed param and wildcard terminal segment
  router.setPath(http::Method::GET, "/p/{}/q/*", OkHandler);
  // Overwrite again to force patternString() on a route with params and wildcard
  router.setPath(http::Method::GET, std::string{"/p/{}/q/*"}, CreatedHandler);

  // Basic asserts to ensure handlers are present
  EXPECT_NE(router.match(http::Method::GET, "/").requestHandler(), nullptr);
  EXPECT_NE(router.match(http::Method::GET, "/p/42/q/x/y").requestHandler(), nullptr);
}

TEST_F(RouterTest, TerminalWildcardMatchesEmptySuffix) {
  // Register wildcard route and ensure matching the parent path (no extra segments)
  router.setPath(http::Method::GET, "/files/*", OkHandler);

  // Matching exactly '/files' should match the wildcard child (terminal wildcard accepts empty suffix)
  auto res = router.match(http::Method::GET, "/files");
  EXPECT_NE(res.requestHandler(), nullptr);
}

TEST_F(RouterTest, ComputePathHandlerEntryReturnsNullOnRedirectSlowPath) {
  // Use Redirect policy and register only the no-slash variant for a pattern route.
  cfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect);
  router = Router(cfg);

  // Register only the no-slash form for a pattern route
  router.setPath(http::Method::GET, "/items/{id}", OkHandler);

  // Matching the opposite form (with trailing slash) should produce a redirect indication
  auto res = router.match(http::Method::GET, "/items/42/");
  EXPECT_EQ(res.requestHandler(), nullptr);
  EXPECT_EQ(res.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::RemoveSlash);
}

TEST_F(RouterTest, AllowedMethodsFastPathChoosesWithSlash) {
  cfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Strict);
  router = Router(cfg);

  // literal-only fast-path: register /lit/ and /lit
  router.setPath(http::Method::GET, "/lit/", OkHandler);
  router.setPath(http::Method::POST, "/lit", CreatedHandler);

  // Query allowed methods for trailing-slash form -> should take into account the slash
  auto bmp = router.allowedMethods("/lit");
  EXPECT_FALSE(http::IsMethodIdxSet(bmp, MethodToIdx(http::Method::GET)));
  EXPECT_TRUE(http::IsMethodIdxSet(bmp, MethodToIdx(http::Method::POST)));

  bmp = router.allowedMethods("/lit/");
  EXPECT_TRUE(http::IsMethodIdxSet(bmp, MethodToIdx(http::Method::GET)));
  EXPECT_FALSE(http::IsMethodIdxSet(bmp, MethodToIdx(http::Method::POST)));
}

TEST_F(RouterTest, AllowedMethodsChoosesNoSlashForStrictSlowPath) {
  cfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Strict);
  router = Router(cfg);

  // Register only the no-slash form for a pattern route and the with-slash for POST
  router.setPath(http::Method::GET, "/items/{id}", OkHandler);
  router.setPath(http::Method::POST, "/items/{id}/", CreatedHandler);

  // Query allowed methods for the no-slash form -> should prefer handlersNoSlash (GET)
  auto bmp = router.allowedMethods("/items/42");
  EXPECT_TRUE(http::IsMethodIdxSet(bmp, MethodToIdx(http::Method::GET)));
  EXPECT_FALSE(http::IsMethodIdxSet(bmp, MethodToIdx(http::Method::POST)));

  bmp = router.allowedMethods("/items/42/");
  EXPECT_FALSE(http::IsMethodIdxSet(bmp, MethodToIdx(http::Method::GET)));
  EXPECT_TRUE(http::IsMethodIdxSet(bmp, MethodToIdx(http::Method::POST)));
}

TEST_F(RouterTest, MissingClosingBraceInParamThrows) {
  EXPECT_THROW(router.setPath(http::Method::GET, "/foo/{bar", OkHandler), std::invalid_argument);
  EXPECT_THROW(router.setPath(http::Method::GET, "/foo/{bar{{}}", OkHandler), std::invalid_argument);
  EXPECT_THROW(router.setPath(http::Method::GET, "/foo/{bar{{}}}}", OkHandler), std::invalid_argument);
  EXPECT_THROW(router.setPath(http::Method::GET, "/foo/{x}}bar", OkHandler), std::invalid_argument);
}

TEST_F(RouterTest, ParamWithLiteralSuffix) {
  router.setPath(http::Method::GET, "/file-{name}.json", OkHandler);

  auto ok = router.match(http::Method::GET, "/file-test.json");
  ASSERT_TRUE(ok.hasHandler());
  EXPECT_EQ(ok.pathParams[0].key, "name");
  EXPECT_EQ(ok.pathParams[0].value, "test");

  auto bad = router.match(http::Method::GET, "/file-test.txt");
  EXPECT_FALSE(bad.hasHandler());
}

TEST_F(RouterTest, StrictRejectsTrailingSlash) {
  RouterConfig cfg;
  cfg.trailingSlashPolicy = RouterConfig::TrailingSlashPolicy::Strict;
  router = Router(cfg);

  router.setPath(http::Method::GET, "/*", OkHandler);

  auto res = router.match(http::Method::GET, "/foo/");
  EXPECT_FALSE(res.hasHandler());
}

TEST_F(RouterTest, NormalizedWithTrailingSlashShouldOverride) {
  RouterConfig cfg;
  cfg.trailingSlashPolicy = RouterConfig::TrailingSlashPolicy::Normalize;
  router = Router(cfg);

  router.setPath(http::Method::GET, "/foo/{bar}", OkHandler);
  router.setPath(http::Method::GET, "/foo/{bar}/", AcceptedHandler);

  auto res1 = router.match(http::Method::GET, "/foo/123");
  EXPECT_TRUE(res1.hasHandler());

  auto res2 = router.match(http::Method::GET, "/foo/123/");
  EXPECT_TRUE(res2.hasHandler());

  EXPECT_EQ((*res1.requestHandler())(dummyReq()).status(), 202);
  EXPECT_EQ((*res2.requestHandler())(dummyReq()).status(), 202);
}

TEST_F(RouterTest, ParamRouteTrailingSlashNormalize) {
  RouterConfig cfg;
  cfg.trailingSlashPolicy = RouterConfig::TrailingSlashPolicy::Normalize;
  router = Router(cfg);

  router.setPath(http::Method::GET, "/foo/{id}", OkHandler);

  auto res = router.match(http::Method::GET, "/foo/123/");
  EXPECT_TRUE(res.hasHandler());
  EXPECT_EQ(res.pathParams.size(), 1U);
  EXPECT_EQ(res.pathParams[0].key, "id");
  EXPECT_EQ(res.pathParams[0].value, "123");
}

TEST_F(RouterTest, ParamAtSegmentStart) { router.setPath(http::Method::GET, "/{id}", OkHandler); }

TEST_F(RouterTest, EmptyRouterPrintsEmptyMessage) {
  std::stringstream ss;
  router.printTree(ss);
  EXPECT_EQ(ss.str(), "<empty router>\n");
}

TEST_F(RouterTest, ParamRoutePrintsExpectedTree) {
  router.setPath(http::Method::GET, "/users/{id}",
                 [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });

  std::stringstream ss;
  router.printTree(ss);

  static constexpr std::string_view kExpected = R"(Radix tree
==========
└─ [STATIC] "/users/"  (hasWildChild)
│   edge <wildcard>
│   │   └─ [PARAM] "{id}"  [ROUTE no-slash]  [handlers]
)";

  EXPECT_EQ(ss.str(), kExpected);
}

TEST_F(RouterTest, DebugOutput) {
  // Tests debug output functionality that was uncovered
  router.setPath(http::Method::GET, "/test", [](const HttpRequest &) { return HttpResponse("test"); });
  router.setPath(http::Method::GET, "/api/{id}", [](const HttpRequest &) { return HttpResponse("api"); });
  router.setPath(http::Method::GET, "/catch/*", [](const HttpRequest &) { return HttpResponse("catch"); });

  // Call the printTree function (covers printNode and indent functions)
  std::ostringstream oss;
  router.printTree(oss);
  const std::string output = oss.str();

  // Verify output was generated (covers printNode and indent functions)
  EXPECT_FALSE(output.empty()) << "Router printTree output should not be empty";
}

TEST_F(RouterTest, EscapedBracesInParameterPattern) {
  // Test escaped braces {{ inside parameter segment - covers line 383-388
  // Pattern: {id}{{suffix means param 'id' followed by literal '{suffix'
  router.setPath(http::Method::GET, "/api/{id}{{literal", OkHandler);

  auto res = router.match(http::Method::GET, "/api/123{literal");
  ASSERT_NE(res.requestHandler(), nullptr);
  EXPECT_FALSE(res.methodNotAllowed);
}

TEST_F(RouterTest, CatchAllWithJustStarPath) {
  // Test registering catch-all route with "/*" and then re-registering "*" portion - covers line 555 branch True
  // This exercises the code path where we match with a wildcard child and path becomes "*"
  router.setPath(http::Method::POST, "/*", OkHandler);
  router.setPath(http::Method::POST, "/", AcceptedHandler);

  auto res1 = router.match(http::Method::POST, "/");
  ASSERT_NE(res1.requestHandler(), nullptr);

  auto res2 = router.match(http::Method::POST, "/test");
  ASSERT_NE(res2.requestHandler(), nullptr);
}

TEST_F(RouterTest, CatchAllRouteWithNullRoute) {
  // Test the branch where pNode->pRoute == nullptr when registering catch-all - covers line 557 branch True
  // First register a catch-all
  router.setPath(http::Method::GET, "/*", OkHandler);
  // Then register the exact same catch-all again with trailing slash variant to hit line 557
  router.setPath(http::Method::GET, "/", AcceptedHandler);

  auto res1 = router.match(http::Method::GET, "/");
  ASSERT_NE(res1.requestHandler(), nullptr);

  auto res2 = router.match(http::Method::GET, "/anything");
  ASSERT_NE(res2.requestHandler(), nullptr);
}

TEST_F(RouterTest, ConflictingParamNamesSamePatternThrows) {
  router.setPath(http::Method::GET, "/items/{id}", OkHandler);
  EXPECT_THROW(router.setPath(http::Method::GET, "/items/{name}", OkHandler), std::logic_error);
}

TEST_F(RouterTest, OverwriteParamRouteUsesLatestHandler) {
  router.setPath(http::Method::GET, "/users/{id}", OkHandler);
  router.setPath(http::Method::GET, "/users/{id}", AcceptedHandler);

  auto res = router.match(http::Method::GET, "/users/42");
  ASSERT_NE(res.requestHandler(), nullptr);
  EXPECT_EQ((*res.requestHandler())(dummyReq()).status(), http::StatusCodeAccepted);
}

TEST_F(RouterTest, OverwriteCatchAllRouteUsesLatestHandler) {
  router.setPath(http::Method::GET, "/files/*", OkHandler);
  router.setPath(http::Method::GET, "/files/*", AcceptedHandler);

  auto res = router.match(http::Method::GET, "/files/archive.tar");
  ASSERT_NE(res.requestHandler(), nullptr);
  EXPECT_EQ((*res.requestHandler())(dummyReq()).status(), http::StatusCodeAccepted);
}

TEST_F(RouterTest, OverwriteStaticLeafUnderParamUsesLatestHandler) {
  router.setPath(http::Method::GET, "/users/{id}/details", OkHandler);
  router.setPath(http::Method::GET, "/users/{id}/details", AcceptedHandler);

  auto res = router.match(http::Method::GET, "/users/7/details");
  ASSERT_NE(res.requestHandler(), nullptr);
  EXPECT_EQ((*res.requestHandler())(dummyReq()).status(), http::StatusCodeAccepted);
}

}  // namespace aeronet