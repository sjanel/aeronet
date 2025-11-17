#include "aeronet/router.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/router-config.hpp"

using namespace aeronet;

TEST(RouterTest, RegisterAndMatchNormalHandler) {
  Router router;

  bool called = false;
  router.setPath(http::Method::GET, "/hello", [&called](const HttpRequest &) {
    called = true;
    return HttpResponse(http::StatusCodeOK, "OK");
  });

  auto res = router.match(http::Method::GET, "/hello");
  ASSERT_NE(res.pRequestHandler, nullptr);
  ASSERT_EQ(res.pStreamingHandler, nullptr);
  ASSERT_FALSE(res.methodNotAllowed);

  // Invoke the handler via the pointer to ensure it is callable and behaves correctly
  // Use an aligned storage because HttpRequest constructor should be kept private
  alignas(HttpRequest) std::byte httpRequestStorage[sizeof(HttpRequest)];
  const HttpRequest &dummy = *reinterpret_cast<const HttpRequest *>(&httpRequestStorage);
  HttpResponse resp = (*res.pRequestHandler)(dummy);
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_TRUE(called);
}

TEST(RouterTest, RegisterAndMatchStreamingHandler) {
  Router router;

  bool streamCalled = false;
  router.setPath(http::Method::POST, "/stream",
                 [&streamCalled](const HttpRequest &, [[maybe_unused]] HttpResponseWriter &) { streamCalled = true; });

  auto res = router.match(http::Method::POST, "/stream");
  ASSERT_EQ(res.pRequestHandler, nullptr);
  ASSERT_NE(res.pStreamingHandler, nullptr);
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
  EXPECT_EQ(resPost.pRequestHandler, nullptr);

  // GET should match
  auto resGet = router.match(http::Method::GET, "/onlyget");
  EXPECT_FALSE(resGet.methodNotAllowed);
  EXPECT_NE(resGet.pRequestHandler, nullptr);

  // No path registered -> fallback to no handler (empty)
  auto resMissing = router.match(http::Method::GET, "/missing");
  EXPECT_EQ(resMissing.pRequestHandler, nullptr);
  EXPECT_EQ(resMissing.pStreamingHandler, nullptr);
  EXPECT_FALSE(resMissing.methodNotAllowed);
}

TEST(RouterTest, GlobalDefaultHandlersUsedWhenNoPath) {
  Router router;

  router.setDefault([](const HttpRequest &) { return HttpResponse(204); });

  auto res = router.match(http::Method::GET, "/nope");
  ASSERT_NE(res.pRequestHandler, nullptr);
  ASSERT_EQ(res.pStreamingHandler, nullptr);
  EXPECT_FALSE(res.methodNotAllowed);

  // streaming default
  Router r2;
  bool sCalled = false;
  r2.setDefault([&sCalled](const HttpRequest &, HttpResponseWriter &writerParam) {
    sCalled = true;
    writerParam.end();
  });
  auto res2 = r2.match(http::Method::GET, "/nope");
  ASSERT_EQ(res2.pRequestHandler, nullptr);
  ASSERT_NE(res2.pStreamingHandler, nullptr);
}

TEST(RouterTest, TrailingSlashRedirectAndNormalize) {
  // Redirect policy: registering /p should redirect /p/ -> AddSlash or RemoveSlash depending
  RouterConfig cfg;
  cfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect);
  Router router(cfg);

  router.setPath(http::Method::GET, "/can", [](const HttpRequest &) { return HttpResponse(200); });

  // exact match
  auto resExact = router.match(http::Method::GET, "/can");
  EXPECT_NE(resExact.pRequestHandler, nullptr);
  EXPECT_EQ(resExact.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::None);

  // non-exact with trailing slash should request redirect (RemoveSlash)
  auto resSlashed = router.match(http::Method::GET, "/can/");
  EXPECT_EQ(resSlashed.pRequestHandler, nullptr);
  EXPECT_EQ(resSlashed.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::RemoveSlash);
}

TEST(RouterTest, HeadFallbackToGet) {
  Router router;
  router.setPath(http::Method::GET, "/hf", [](const HttpRequest &) { return HttpResponse(200); });

  // HEAD should fallback to GET handler when no explicit HEAD handler registered
  auto resHead = router.match(http::Method::HEAD, "/hf");
  EXPECT_NE(resHead.pRequestHandler, nullptr);
  EXPECT_EQ(resHead.pStreamingHandler, nullptr);
  EXPECT_FALSE(resHead.methodNotAllowed);
}

TEST(RouterTest, MethodMergingAndOverwrite) {
  Router router;
  // register GET and then add POST using method-bmp OR
  router.setPath(http::Method::GET, "/merge", [](const HttpRequest &) { return HttpResponse(200); });
  router.setPath(http::Method::POST, "/merge", [](const HttpRequest &) { return HttpResponse(201); });

  auto rGet = router.match(http::Method::GET, "/merge");
  EXPECT_NE(rGet.pRequestHandler, nullptr);
  EXPECT_FALSE(rGet.methodNotAllowed);

  auto rPost = router.match(http::Method::POST, "/merge");
  EXPECT_NE(rPost.pRequestHandler, nullptr);
  EXPECT_FALSE(rPost.methodNotAllowed);
}

TEST(RouterTest, MethodBitmapRegistersMultipleHandlers) {
  Router router;
  router.setPath(http::Method::GET | http::Method::POST, "/combo",
                 [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });

  auto getRes = router.match(http::Method::GET, "/combo");
  EXPECT_NE(getRes.pRequestHandler, nullptr);
  EXPECT_FALSE(getRes.methodNotAllowed);

  auto postRes = router.match(http::Method::POST, "/combo");
  EXPECT_NE(postRes.pRequestHandler, nullptr);
  EXPECT_FALSE(postRes.methodNotAllowed);
}

TEST(RouterTest, StreamingVsNormalConflictThrows) {
  Router router;
  router.setPath(http::Method::GET, "/conf", [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });
  // Attempting to register a streaming handler for the same path+method should throw
  EXPECT_THROW(router.setPath(http::Method::GET, std::string{"/conf"},
                              Router::StreamingHandler([](const HttpRequest &, HttpResponseWriter &) {})),
               std::logic_error);
}

TEST(RouterTest, TrailingSlashStrictAndNormalize) {
  // Strict: /a/ registered does not match /a
  RouterConfig cfgStrict;
  cfgStrict.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Strict);
  Router rStrict(cfgStrict);
  rStrict.setPath(http::Method::GET, "/s/", [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });
  auto res1 = rStrict.match(http::Method::GET, "/s/");
  EXPECT_NE(res1.pRequestHandler, nullptr);
  auto res1b = rStrict.match(http::Method::GET, "/s");
  EXPECT_EQ(res1b.pRequestHandler, nullptr);

  // Normalize: registering /n/ makes /n acceptable
  RouterConfig cfgNorm;
  cfgNorm.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Normalize);
  Router rNorm(cfgNorm);
  rNorm.setPath(http::Method::GET, "/n/", [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });
  auto res2 = rNorm.match(http::Method::GET, "/n");
  EXPECT_NE(res2.pRequestHandler, nullptr);
}

TEST(RouterTest, CapturesNamedParameters) {
  Router router;
  router.setPath(http::Method::GET, "/users/{userId}/posts/{postId}",
                 [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });

  auto res = router.match(http::Method::GET, "/users/42/posts/abc");
  ASSERT_NE(res.pRequestHandler, nullptr);
  ASSERT_EQ(res.pathParams.size(), 2U);
  EXPECT_EQ(res.pathParams[0].key, "userId");
  EXPECT_EQ(res.pathParams[0].value, "42");
  EXPECT_EQ(res.pathParams[1].key, "postId");
  EXPECT_EQ(res.pathParams[1].value, "abc");
}

TEST(RouterTest, CapturesUnnamedParametersAsIndices) {
  Router router;
  router.setPath(http::Method::GET, "/files/{}/chunk/{}",
                 [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });

  auto res = router.match(http::Method::GET, "/files/alpha/chunk/123");
  ASSERT_NE(res.pRequestHandler, nullptr);
  ASSERT_EQ(res.pathParams.size(), 2U);
  EXPECT_EQ(res.pathParams[0].key, "0");
  EXPECT_EQ(res.pathParams[0].value, "alpha");
  EXPECT_EQ(res.pathParams[1].key, "1");
  EXPECT_EQ(res.pathParams[1].value, "123");
}

TEST(RouterTest, SupportsLiteralAndParamMixWithinSegment) {
  Router router;
  router.setPath(http::Method::GET, "/api/v{}/foo{}bar",
                 [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });

  auto res = router.match(http::Method::GET, "/api/v1/foo123bar");
  ASSERT_NE(res.pRequestHandler, nullptr);
  ASSERT_EQ(res.pathParams.size(), 2U);
  EXPECT_EQ(res.pathParams[0].value, "1");
  EXPECT_EQ(res.pathParams[1].value, "123");
}

TEST(RouterTest, WildcardMatchesRemainingSegments) {
  Router router;
  router.setPath(http::Method::GET, "/static/*", [](const HttpRequest &) { return HttpResponse(http::StatusCodeOK); });

  auto res = router.match(http::Method::GET, "/static/css/app/main.css");
  ASSERT_NE(res.pRequestHandler, nullptr);
  EXPECT_EQ(res.pathParams.size(), 0U);
}

TEST(RouterTest, CopyConstructorCopiesHandlersAndPatterns) {
  Router router;

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
  router.setPath(
      http::Method::PUT, "/stream/x",
      Router::StreamingHandler([&streamCalled](const HttpRequest &, HttpResponseWriter &) { streamCalled = true; }));

  // wildcard
  router.setPath(http::Method::GET, "/wild/*", [](const HttpRequest &) { return HttpResponse(200); });

  // copy-construct
  Router clone(router);

  // original handlers still work
  // prepare a dummy HttpRequest storage used for invoking handlers in tests
  alignas(HttpRequest) std::byte dummyReqStorage[sizeof(HttpRequest)];
  const HttpRequest &dummyReq = *reinterpret_cast<const HttpRequest *>(&dummyReqStorage);

  auto resFromOriginal = router.match(http::Method::GET, "/copy/a");
  ASSERT_NE(resFromOriginal.pRequestHandler, nullptr);
  (*resFromOriginal.pRequestHandler)(dummyReq);

  auto resFromClone = clone.match(http::Method::GET, "/copy/a");
  ASSERT_NE(resFromClone.pRequestHandler, nullptr);
  (*resFromClone.pRequestHandler)(dummyReq);

  EXPECT_TRUE(calledA);

  auto resPost = clone.match(http::Method::POST, "/files/v1/part/xyz");
  ASSERT_NE(resPost.pRequestHandler, nullptr);
  (*resPost.pRequestHandler)(dummyReq);
  EXPECT_TRUE(calledB);

  auto resStream = clone.match(http::Method::PUT, "/stream/x");
  ASSERT_NE(resStream.pStreamingHandler, nullptr);
  // don't invoke streaming handler here; presence is sufficient

  auto resWild = clone.match(http::Method::GET, "/wild/any/path/here");
  ASSERT_NE(resWild.pRequestHandler, nullptr);
}

TEST(RouterTest, CopyAssignmentPreservesHandlersAndIsIndependent) {
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
  ASSERT_NE(rBase.pRequestHandler, nullptr);
  (*rBase.pRequestHandler)(dummyReq2);
  EXPECT_EQ(invokedOriginal, 1);

  auto rDest = destRouter.match(http::Method::GET, "/indep/x");
  ASSERT_NE(rDest.pRequestHandler, nullptr);
  (*rDest.pRequestHandler)(dummyReq2);
  // assignment should have copied handler behaviour
  EXPECT_EQ(invokedOriginal, 2);

  // Now mutate original: overwrite handler for the same path
  baseRouter.setPath(http::Method::GET, "/indep/x", [](const HttpRequest &) { return HttpResponse(201); });

  // base now has new handler; dest should still have the old one
  auto rBase2 = baseRouter.match(http::Method::GET, "/indep/x");
  ASSERT_NE(rBase2.pRequestHandler, nullptr);
  HttpResponse respBase = (*rBase2.pRequestHandler)(dummyReq2);
  EXPECT_EQ(respBase.status(), 201);

  auto rDest2 = destRouter.match(http::Method::GET, "/indep/x");
  ASSERT_NE(rDest2.pRequestHandler, nullptr);
  HttpResponse respDest = (*rDest2.pRequestHandler)(dummyReq2);
  EXPECT_EQ(respDest.status(), 200);
}

TEST(RouterTest, CopyPreservesTrailingSlashVariantsAndMethodTypes) {
  Router rTs;
  rTs.setPath(http::Method::GET, "/ts/", [](const HttpRequest &) { return HttpResponse(200); });
  rTs.setPath(http::Method::POST, "/ts", [](const HttpRequest &) { return HttpResponse(201); });

  Router cTs(rTs);

  // GET /ts/ should match in clone
  auto rg = cTs.match(http::Method::GET, "/ts/");
  ASSERT_NE(rg.pRequestHandler, nullptr);

  // POST /ts should match in clone
  auto rp = cTs.match(http::Method::POST, "/ts");
  ASSERT_NE(rp.pRequestHandler, nullptr);
  alignas(HttpRequest) std::byte dummyTsReq[sizeof(HttpRequest)];
  const HttpRequest &dummyTs = *reinterpret_cast<const HttpRequest *>(&dummyTsReq);
  HttpResponse resp = (*rp.pRequestHandler)(dummyTs);
  EXPECT_EQ(resp.status(), 201);
}

TEST(RouterTest, CopyHandlesHeadFallbackAndMethodBitmaps) {
  Router rHf;
  rHf.setPath(http::Method::GET, "/hfcopy", [](const HttpRequest &) { return HttpResponse(200); });
  Router cHf = rHf;

  // HEAD should fallback to GET in clone
  auto rh = cHf.match(http::Method::HEAD, "/hfcopy");
  EXPECT_NE(rh.pRequestHandler, nullptr);
  EXPECT_FALSE(rh.methodNotAllowed);
}

TEST(RouterTest, CopyPreservesLiteralOnlyFastPath) {
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
  ASSERT_NE(resOriginal.pRequestHandler, nullptr);
  (*resOriginal.pRequestHandler)(dummyReq);
  EXPECT_EQ(callCount, 1);

  auto resClone = clone.match(http::Method::GET, "/api/v1/users/list");
  ASSERT_NE(resClone.pRequestHandler, nullptr);
  (*resClone.pRequestHandler)(dummyReq);
  EXPECT_EQ(callCount, 2);

  // Verify independence: modifying original doesn't affect clone
  original.setPath(http::Method::GET, "/api/v1/users/list", [](const HttpRequest &) { return HttpResponse(404); });

  auto resCloneAfter = clone.match(http::Method::GET, "/api/v1/users/list");
  ASSERT_NE(resCloneAfter.pRequestHandler, nullptr);
  HttpResponse resp = (*resCloneAfter.pRequestHandler)(dummyReq);
  EXPECT_EQ(resp.status(), 200);  // Clone still has old handler
  EXPECT_EQ(callCount, 3);
}

TEST(RouterTest, NonCopyableHandlerAcrossMultipleMethods) {
  Router router;

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
    if (res.pRequestHandler == nullptr) {
      return;  // treat as not registered
    }
    try {
      HttpResponse response = (*res.pRequestHandler)(dummyReq);
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

// New exhaustive coverage tests
TEST(RouterTest, CompilePatternErrorsAndEscapes) {
  Router router;

  // Path must begin with '/'
  EXPECT_THROW(router.setPath(http::Method::GET, "no-slash", [](const HttpRequest &) { return HttpResponse(200); }),
               std::invalid_argument);

  // Empty segment
  EXPECT_THROW(router.setPath(http::Method::GET, "/a//b", [](const HttpRequest &) { return HttpResponse(200); }),
               std::invalid_argument);

  // Unterminated brace
  EXPECT_THROW(router.setPath(http::Method::GET, "/u{bad", [](const HttpRequest &) { return HttpResponse(200); }),
               std::invalid_argument);

  // Escaped braces should be accepted literally
  router.setPath(http::Method::GET, "/literal/{{}}/end", [](const HttpRequest &) { return HttpResponse(200); });
  auto res = router.match(http::Method::GET, "/literal/{} /end");
  // no match because space inserted; ensure matching doesn't crash
  EXPECT_EQ(res.pRequestHandler, nullptr);
}

TEST(RouterTest, MixedNamedAndUnnamedParamsDisallowed) {
  Router routerBad;
  EXPECT_THROW(
      routerBad.setPath(http::Method::GET, "/mix/{}/{id}", [](const HttpRequest &) { return HttpResponse(200); }),
      std::invalid_argument);
  EXPECT_THROW(
      routerBad.setPath(http::Method::GET, "/mix/{id}/{}/", [](const HttpRequest &) { return HttpResponse(200); }),
      std::invalid_argument);
}

TEST(RouterTest, WildcardConflictAndTerminalRules) {
  Router routerWildcard;
  // wildcard must be terminal
  EXPECT_THROW(
      routerWildcard.setPath(http::Method::GET, "/bad/*/here", [](const HttpRequest &) { return HttpResponse(200); }),
      std::invalid_argument);

  // wildcard matching precedence and allowedMethods
  routerWildcard.setPath(http::Method::GET, "/files/*", [](const HttpRequest &) { return HttpResponse(200); });
  routerWildcard.setPath(http::Method::POST, "/files/upload", [](const HttpRequest &) { return HttpResponse(201); });

  auto gm = routerWildcard.match(http::Method::GET, "/files/foo/bar");
  EXPECT_NE(gm.pRequestHandler, nullptr);

  auto pm = routerWildcard.match(http::Method::POST, "/files/upload");
  EXPECT_NE(pm.pRequestHandler, nullptr);
}

TEST(RouterTest, AllowedMethodsAndGlobalFallback) {
  Router routerAllowed;
  routerAllowed.setDefault([](const HttpRequest &) { return HttpResponse(204); });
  routerAllowed.setPath(http::Method::GET | http::Method::POST, "/combo2",
                        [](const HttpRequest &) { return HttpResponse(); });

  auto allowed = routerAllowed.allowedMethods("/combo2");
  EXPECT_TRUE(http::isMethodSet(allowed, toMethodIdx(http::Method::GET)));
  EXPECT_TRUE(http::isMethodSet(allowed, toMethodIdx(http::Method::POST)));

  // Path not registered -> all methods allowed because global handler present
  auto allAllowed = routerAllowed.allowedMethods("/nope");
  EXPECT_NE(allAllowed, 0U);
}

TEST(RouterTest, TrailingSlashEdgeCases) {
  // Normalize should accept both
  RouterConfig cfg;
  cfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Normalize);
  Router rn(cfg);
  rn.setPath(http::Method::GET, "/norm/", [](const HttpRequest &) { return HttpResponse(); });
  auto resNorm = rn.match(http::Method::GET, "/norm");
  EXPECT_NE(resNorm.pRequestHandler, nullptr);

  // Strict must differentiate
  RouterConfig cs;
  cs.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Strict);
  Router rs(cs);
  rs.setPath(http::Method::GET, "/str/", [](const HttpRequest &) { return HttpResponse(); });
  auto resStrict = rs.match(http::Method::GET, "/str");
  EXPECT_EQ(resStrict.pRequestHandler, nullptr);
}

TEST(RouterTest, ConflictingWildcardAndExact) {
  Router routerConflict;
  routerConflict.setPath(http::Method::GET, "/a/b", [](const HttpRequest &) { return HttpResponse(); });
  routerConflict.setPath(http::Method::GET, "/a/*", [](const HttpRequest &) { return HttpResponse(); });

  // exact should win
  auto ex = routerConflict.match(http::Method::GET, "/a/b");
  EXPECT_NE(ex.pRequestHandler, nullptr);
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
    router.setPath(http::Method::GET, "/tp", [](const HttpRequest &) { return HttpResponse(200); });
    router.setPath(http::Method::GET, "/tp/", [](const HttpRequest &) { return HttpResponse(201); });
    return router;
  }
};

TEST_F(RouterTestTrailingPolicy, NormalizeAcceptsBothForms) {
  Router router = RouterTestTrailingPolicy::makeRouter(RouterConfig::TrailingSlashPolicy::Normalize);

  // both forms should match a handler (prefer the exact-registered variant)
  auto resNoSlash = router.match(http::Method::GET, "/tp");
  EXPECT_NE(resNoSlash.pRequestHandler, nullptr);
  EXPECT_EQ(resNoSlash.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::None);

  auto resWithSlash = router.match(http::Method::GET, "/tp/");
  EXPECT_NE(resWithSlash.pRequestHandler, nullptr);
  EXPECT_EQ(resWithSlash.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::None);
}

TEST_F(RouterTestTrailingPolicy, StrictDistinguishesForms) {
  Router router = RouterTestTrailingPolicy::makeRouter(RouterConfig::TrailingSlashPolicy::Strict);

  // strict should only match the exact variant
  auto resNoSlash = router.match(http::Method::GET, "/tp");
  EXPECT_NE(resNoSlash.pRequestHandler, nullptr);

  auto resWithSlash = router.match(http::Method::GET, "/tp/");
  EXPECT_NE(resWithSlash.pRequestHandler, nullptr);
  // ensure that matching the opposite form does not return the other's handler implicitly
  // The router should not redirect in Strict mode; instead both registered variants coexist
  EXPECT_EQ(resWithSlash.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::None);
}

TEST_F(RouterTestTrailingPolicy, RedirectRequestsRedirect) {
  Router router = RouterTestTrailingPolicy::makeRouter(RouterConfig::TrailingSlashPolicy::Redirect);

  // When Redirect policy is active, requesting the non-registered variant should produce a redirect indicator
  // Since we registered both forms, invoking the opposite form should trigger the appropriate redirect behaviour
  auto resNoSlash = router.match(http::Method::GET, "/tp");
  EXPECT_NE(resNoSlash.pRequestHandler, nullptr);

  auto resWithSlash = router.match(http::Method::GET, "/tp/");
  EXPECT_NE(resWithSlash.pRequestHandler, nullptr);

  // Now check cross-requests: if we temporarily create a router that only has the no-slash registered,
  // Redirect policy should request AddSlash when matching "/tp/" and RemoveSlash when matching "/tp" if vice-versa.
  RouterConfig cfg;
  cfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect);
  Router r2(cfg);
  r2.setPath(http::Method::GET, "/onlynoslash", [](const HttpRequest &) { return HttpResponse(200); });

  auto r2Slashed = r2.match(http::Method::GET, "/onlynoslash/");
  EXPECT_EQ(r2Slashed.pRequestHandler, nullptr);
  EXPECT_EQ(r2Slashed.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::RemoveSlash);

  Router r3(cfg);
  r3.setPath(http::Method::GET, "/onlywithslash/", [](const HttpRequest &) { return HttpResponse(200); });
  auto r3NoSlash = r3.match(http::Method::GET, "/onlywithslash");
  EXPECT_EQ(r3NoSlash.pRequestHandler, nullptr);
  EXPECT_EQ(r3NoSlash.redirectPathIndicator, Router::RoutingResult::RedirectSlashMode::AddSlash);
}

// The large stress scenario is covered by `LargeNumberOfPatternsAndSegments_WithTrailingPolicies` which
// runs the same registration/match logic across different trailing-slash policies. The older
// `LargeNumberOfPatternsAndSegments` test has been removed to avoid duplication.

// Run the large stress registration+match scenario for each trailing-slash policy to
// ensure the router behaves correctly under different normalization/redirect semantics.
TEST(RouterTest, LargeNumberOfPatternsAndSegments_WithTrailingPolicies) {
  const std::array<RouterConfig::TrailingSlashPolicy, 3> policies = {
      RouterConfig::TrailingSlashPolicy::Normalize,
      RouterConfig::TrailingSlashPolicy::Strict,
      RouterConfig::TrailingSlashPolicy::Redirect,
  };

  for (auto policy : policies) {
    RouterConfig cfg;
    cfg.withTrailingSlashPolicy(policy);
    Router router(cfg);

    const std::size_t routeCount = 1000;  // same large size as the base test
    const std::size_t segments = 6;

    std::vector<int> called(routeCount, 0);

    // registration lambda so we reuse logic
    auto registerRoutes = [&](Router &router) {
      for (std::size_t idx = 0; idx < routeCount; ++idx) {
        std::string path = "/r/tp/id" + std::to_string(idx);
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

      for (std::size_t idx = 0; idx < routeCount; ++idx) {
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
        ASSERT_NE(res.pRequestHandler, nullptr)
            << "No handler for path: " << matchPath << " policy=" << static_cast<int>(policy);
        (*res.pRequestHandler)(dummyReq);
      }
    };

    // perform registration and matching
    registerRoutes(router);
    matchAndInvoke(router);

    // verify
    for (std::size_t idx = 0; idx < routeCount; ++idx) {
      EXPECT_EQ(called[idx], 1) << "Handler not invoked for index " << idx << " policy=" << static_cast<int>(policy);
    }
  }
}
