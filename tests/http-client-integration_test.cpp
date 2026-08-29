// Integration tests that drive a full aeronet server through the aeronet HttpClient.
// This exercises both ends of the library against each other (request building, chunked
// streaming, keep-alive reuse, query/headers) over real sockets.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/aeronet.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/test_server_fixture.hpp"

#ifdef AERONET_ENABLE_RESPONSE_CACHE
#include <atomic>

#include "aeronet/http-status-code.hpp"
#include "aeronet/response-cache.hpp"
#endif

namespace aeronet {
namespace {

#ifdef AERONET_ENABLE_RESPONSE_CACHE
ResponseCacheConfig TinyCacheConfig() {
  ResponseCacheConfig config;
  config.maxEntries = 1U;
  config.maxMemoryBytes = 64U * 1024U;
  config.maxEntryBytes = 32U * 1024U;
  return config;
}

ResponseCacheConfig MemoryCacheConfig() {
  ResponseCacheConfig config;
  config.maxEntries = 100U;
  config.maxMemoryBytes = 32768U;
  config.maxEntryBytes = 32768U;
  return config;
}

const ResponseCache cache;
const ResponseCache tinyCache(TinyCacheConfig());
const ResponseCache memoryCache(MemoryCacheConfig());
#endif

test::TestServer ts;

[[nodiscard]] std::string Url(std::string_view path) {
  return "http://127.0.0.1:" + std::to_string(ts.port()) + std::string(path);
}

class HttpClientIntegration : public ::testing::Test {
 protected:
  void SetUp() override {
    ts.resetRouter([&](Router& router) {
      router.setPath(http::Method::GET, "/echo-query", [](const HttpRequestView& req) {
        return req.makeResponse(http::StatusCodeOK, req.queryParamValueOrEmpty("msg"), "text/plain");
      });
      router.setPath(http::Method::POST, "/upload", [](const HttpRequestView& req) {
        return req.makeResponse(http::StatusCodeOK, std::to_string(req.body().size()), "text/plain");
      });
      router.setPath(http::Method::GET, "/headers", [](const HttpRequestView& req) {
        auto resp = req.makeResponse(http::StatusCodeOK, "ok", "text/plain");
        resp.headerAddLine("x-custom", "custom-value");
        return resp;
      });
      // Streaming handler -> Transfer-Encoding: chunked on the wire.
      router.setPath(http::Method::GET, "/stream", [](const HttpRequestView&, HttpResponseWriter& writer) {
        writer.status(http::StatusCodeOK);
        writer.contentType("text/plain");
        writer.writeBody("alpha-");
        writer.writeBody("beta-");
        writer.writeBody("gamma");
        writer.end();
      });
    });
  }
};

TEST_F(HttpClientIntegration, QueryParamRoundTrip) {
  HttpClient client;
  auto resp = client.get(Url("/echo-query?msg=hello-world")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "hello-world");
}

TEST_F(HttpClientIntegration, LargeBodyUpload) {
  HttpClient client;
  std::string payload(256UL * 1024UL, 'Z');  // 256 KiB, exercises multi-write / partial writes
  auto resp = client.post(Url("/upload"), payload, "application/octet-stream").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), std::to_string(payload.size()));
}

TEST_F(HttpClientIntegration, ReadsCustomResponseHeader) {
  HttpClient client;
  auto resp = client.get(Url("/headers")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.headerValueOrEmpty("x-custom"), "custom-value");
  EXPECT_EQ(resp.bodyInMemory(), "ok");
}

TEST_F(HttpClientIntegration, DecodesChunkedStreamingResponse) {
  HttpClient client;
  auto resp = client.get(Url("/stream")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "alpha-beta-gamma");  // chunked framing de-framed by the client
}

TEST_F(HttpClientIntegration, KeepAliveAcrossManyRequests) {
  HttpClient client;
  for (int i = 0; i < 25; ++i) {
    auto resp = client.get(Url("/echo-query?msg=k")).value();
    ASSERT_EQ(resp.status(), 200) << "iteration " << i;
    ASSERT_EQ(resp.bodyInMemory(), "k");
  }
}

TEST_F(HttpClientIntegration, MixedMethodsReuseClient) {
  HttpClient client;
  EXPECT_EQ(client.get(Url("/echo-query?msg=a")).value().bodyInMemory(), "a");
  EXPECT_EQ(client.post(Url("/upload"), "1234", "text/plain").value().bodyInMemory(), "4");
  EXPECT_EQ(client.get(Url("/stream")).value().bodyInMemory(), "alpha-beta-gamma");
}

TEST_F(HttpClientIntegration, IdlePooledConnectionExpires) {
  HttpClientConfig cfg;
  cfg.withKeepAliveTimeout(std::chrono::milliseconds{30});  // expire pooled connections quickly
  HttpClient client(cfg);

  EXPECT_EQ(client.get(Url("/echo-query?msg=first")).value().bodyInMemory(), "first");
  std::this_thread::sleep_for(std::chrono::milliseconds{80});  // exceed the client idle limit
  // The pooled connection is now considered stale and dropped; a fresh one transparently serves this.
  EXPECT_EQ(client.get(Url("/echo-query?msg=second")).value().bodyInMemory(), "second");
}

#ifdef AERONET_ENABLE_RESPONSE_CACHE

using namespace std::chrono_literals;

class ResponseCacheIntegration : public ::testing::Test {
 protected:
  void SetUp() override {
    ts.resetRouter([this](Router& router) {
      router
          .setPath(http::Method::GET, "/basic",
                   [this](const HttpRequestView& request) {
                     const unsigned call = ++basicCalls;
                     auto response = request.makeResponse(http::StatusCodeOK, "basic-" + std::to_string(call));
                     response.headerAddLine(http::CacheControl, "public, max-age=60");
                     return response;
                   })
          .after([this](const HttpRequestView&, HttpResponse& response) { response.header("x-post-run", ++postRuns); })
          .cache(cache);

      router
          .setPath(http::Method::GET, "/query",
                   [this](const HttpRequestView& request) {
                     ++queryCalls;
                     auto response = request.makeResponse(request.queryParamValueOrEmpty("value"));
                     response.headerAddLine(http::CacheControl, "max-age=60");
                     return response;
                   })
          .cache(cache);

      router
          .setPath(http::Method::GET, "/vary",
                   [this](const HttpRequestView& request) {
                     ++varyCalls;
                     auto response =
                         request.makeResponse(request.hasHeader("x-flavor") ? request.headerValueOrEmpty("x-flavor")
                                                                            : std::string_view{"<absent>"});
                     response.headerAddLine(http::CacheControl, "max-age=60");
                     response.headerAddLine(http::Vary, "X-Flavor");
                     return response;
                   })
          .cache(cache);

      router
          .setPath(http::Method::GET, "/vary-policy",
                   [this](const HttpRequestView& request) {
                     const unsigned call = ++varyPolicyCalls;
                     auto response = request.makeResponse("vary-policy-" + std::to_string(call));
                     response.headerAddLine(http::CacheControl, "max-age=60");
                     if (call > 1U) {
                       response.headerAddLine(http::Vary, "*");
                     }
                     return response;
                   })
          .cache(cache);

      router
          .setPath(http::Method::GET, "/set-cookie",
                   [this](const HttpRequestView& request) {
                     const unsigned call = ++setCookieCalls;
                     auto response = request.makeResponse("set-cookie-" + std::to_string(call));
                     response.headerAddLine(http::CacheControl, "max-age=60");
                     if (call > 1U) {
                       response.headerAddLine(http::SetCookie, "session=private");
                     }
                     return response;
                   })
          .cache(cache);

      router
          .setPath(http::Method::GET, "/etag",
                   [this](const HttpRequestView& request) {
                     ++etagCalls;
                     auto response = request.makeResponse("etag-body");
                     response.headerAddLine(http::CacheControl, "max-age=60");
                     response.headerAddLine(http::ETag, "\"version,1\"");
                     return response;
                   })
          .cache(cache);

      router
          .setPath(http::Method::GET, "/refresh",
                   [this](const HttpRequestView& request) {
                     const unsigned call = ++refreshCalls;
                     auto response = request.makeResponse("refresh-" + std::to_string(call));
                     response.headerAddLine(http::CacheControl, "max-age=60");
                     return response;
                   })
          .cache(cache);

      router
          .setPath(http::Method::GET, "/expires",
                   [this](const HttpRequestView& request) {
                     const unsigned call = ++expirationCalls;
                     auto response = request.makeResponse("expires-" + std::to_string(call));
                     response.headerAddLine(http::CacheControl, "max-age=1");
                     return response;
                   })
          .cache(cache);

      router
          .setPath(http::Method::GET, "/sensitive",
                   [this](const HttpRequestView& request) {
                     ++sensitiveCalls;
                     auto response = request.makeResponse("sensitive");
                     response.headerAddLine(http::CacheControl, "max-age=60");
                     return response;
                   })
          .cache(cache);

      for (const std::string_view directive : {"no-store", "no-cache", "private", "max-age=0"}) {
        const std::string path = "/directive/" + std::string(directive);
        router
            .setPath(http::Method::GET, path,
                     [this, directive](const HttpRequestView& request) {
                       ++directiveCalls;
                       auto response = request.makeResponse(directive);
                       response.headerAddLine(http::CacheControl, directive);
                       return response;
                     })
            .cache(cache);
      }

      router
          .setPath(http::Method::GET, "/lru",
                   [this](const HttpRequestView& request) {
                     const bool isA = request.queryParamValueOrEmpty("item") == "a";
                     const unsigned call = isA ? ++lruACalls : ++lruBCalls;
                     auto response = request.makeResponse(std::string(isA ? "a-" : "b-") + std::to_string(call));
                     response.headerAddLine(http::CacheControl, "max-age=60");
                     return response;
                   })
          .cache(tinyCache);

      router
          .setPath(http::Method::GET, "/memory",
                   [this](const HttpRequestView& request) {
                     const bool isA = request.queryParamValueOrEmpty("item") == "a";
                     if (isA) {
                       ++memoryACalls;
                     } else {
                       ++memoryBCalls;
                     }
                     auto response = request.makeResponse(std::string(16000U, isA ? 'a' : 'b'));
                     response.headerAddLine(http::CacheControl, "max-age=60");
                     return response;
                   })
          .cache(memoryCache);

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
      router
          .setPath(http::Method::GET, "/async",
                   [this](HttpRequestView& request) -> RequestTask<HttpResponse> {
                     const unsigned call = ++asyncCalls;
                     auto response = request.makeResponse("async-" + std::to_string(call));
                     response.headerAddLine(http::CacheControl, "max-age=60");
                     co_return response;
                   })
          .cache(cache);
#endif

#ifdef AERONET_ENABLE_HTTP2
      router
          .setPath(http::Method::GET, "/h2",
                   [this](const HttpRequestView& request) {
                     const unsigned call = ++h2Calls;
                     auto response = request.makeResponse("h2-" + std::to_string(call));
                     response.headerAddLine(http::CacheControl, "max-age=60");
                     return response;
                   })
          .cache(cache);
#endif
    });
  }

  void TearDown() override { ts.resetRouter(); }

  static HttpResponse Value(HttpClientResult result) {
    EXPECT_TRUE(result.has_value());
    return result.has_value() ? std::move(*result) : HttpResponse(http::StatusCodeInternalServerError);
  }

  ResponseCacheStats CacheStats(std::string_view routePath) {
    ResponseCacheStats result;
    bool found{};
    ts.postRouterUpdate([&](Router& router) {
      if (ResponseCache* routeCache = router.match(http::Method::GET, routePath).responseCache()) {
        result = routeCache->stats();
        found = true;
      }
    });
    EXPECT_TRUE(found);
    return result;
  }

  std::size_t InvalidateCachePath(std::string_view routePath, std::string_view cachedPath) {
    std::size_t removed{};
    bool found{};
    ts.postRouterUpdate([&](Router& router) {
      if (ResponseCache* routeCache = router.match(http::Method::GET, routePath).responseCache()) {
        removed = routeCache->invalidatePath(cachedPath);
        found = true;
      }
    });
    EXPECT_TRUE(found);
    return removed;
  }

  void ClearCache(std::string_view routePath) {
    bool found{};
    ts.postRouterUpdate([&](Router& router) {
      if (ResponseCache* routeCache = router.match(http::Method::GET, routePath).responseCache()) {
        routeCache->clear();
        found = true;
      }
    });
    EXPECT_TRUE(found);
  }

  std::atomic_uint basicCalls{};
  std::atomic_uint postRuns{};
  std::atomic_uint queryCalls{};
  std::atomic_uint varyCalls{};
  std::atomic_uint varyPolicyCalls{};
  std::atomic_uint setCookieCalls{};
  std::atomic_uint etagCalls{};
  std::atomic_uint refreshCalls{};
  std::atomic_uint expirationCalls{};
  std::atomic_uint sensitiveCalls{};
  std::atomic_uint directiveCalls{};
  std::atomic_uint lruACalls{};
  std::atomic_uint lruBCalls{};
  std::atomic_uint memoryACalls{};
  std::atomic_uint memoryBCalls{};
  std::atomic_uint asyncCalls{};
  std::atomic_uint h2Calls{};
};

TEST_F(ResponseCacheIntegration, ReusesHandlerPrototypeAndReappliesResponseMiddleware) {
  HttpClient client;
  const auto first = Value(client.get(Url("/basic")));
  const auto second = Value(client.get(Url("/basic")));

  EXPECT_EQ(first.bodyInMemory(), "basic-1");
  EXPECT_EQ(second.bodyInMemory(), "basic-1");
  EXPECT_EQ(first.headerValueOrEmpty("x-post-run"), "1");
  EXPECT_EQ(second.headerValueOrEmpty("x-post-run"), "2");
  EXPECT_EQ(basicCalls, 1U);
  EXPECT_EQ(postRuns, 2U);
  EXPECT_EQ(CacheStats("/basic").hits, 1U);
}

TEST_F(ResponseCacheIntegration, CachesHeadSizeOnlyResponsesWithoutRetainingABody) {
  HttpClient client;
  const auto first = Value(client.head(Url("/basic")));
  const auto second = Value(client.head(Url("/basic")));

  EXPECT_EQ(first.status(), http::StatusCodeOK);
  EXPECT_EQ(second.status(), http::StatusCodeOK);
  EXPECT_TRUE(first.bodyInMemory().empty());
  EXPECT_TRUE(second.bodyInMemory().empty());
  EXPECT_EQ(basicCalls, 1U);
  EXPECT_EQ(postRuns, 2U);
}

TEST_F(ResponseCacheIntegration, KeysIncludeOrderedQueryAndVaryRequestHeaders) {
  HttpClient client;
  EXPECT_EQ(Value(client.get(Url("/query?value=one"))).bodyInMemory(), "one");
  EXPECT_EQ(Value(client.get(Url("/query?value=one"))).bodyInMemory(), "one");
  EXPECT_EQ(Value(client.get(Url("/query?value=two"))).bodyInMemory(), "two");
  EXPECT_EQ(queryCalls, 2U);

  auto flavored = [&](std::string_view flavor) {
    auto request = client.makeRequest(http::Method::GET, Url("/vary"));
    request.headerAddLine("x-flavor", flavor);
    return Value(client.request(std::move(request)));
  };
  EXPECT_EQ(flavored("vanilla").bodyInMemory(), "vanilla");
  EXPECT_EQ(flavored("chocolate").bodyInMemory(), "chocolate");
  EXPECT_EQ(flavored("vanilla").bodyInMemory(), "vanilla");
  EXPECT_EQ(Value(client.get(Url("/vary"))).bodyInMemory(), "<absent>");
  EXPECT_TRUE(flavored("").bodyInMemory().empty());
  EXPECT_EQ(Value(client.get(Url("/vary"))).bodyInMemory(), "<absent>");
  EXPECT_TRUE(flavored("").bodyInMemory().empty());
  EXPECT_EQ(varyCalls, 4U);
}

TEST_F(ResponseCacheIntegration, AppliesWeakIfNoneMatchValidationOnHits) {
  HttpClient client;
  EXPECT_EQ(Value(client.get(Url("/etag"))).status(), http::StatusCodeOK);

  auto request = client.makeRequest(http::Method::GET, Url("/etag"));
  request.headerAddLine(http::IfNoneMatch, "\"other\", W/\"version,1\"");
  const auto response = Value(client.request(std::move(request)));
  EXPECT_EQ(response.status(), http::StatusCodeNotModified);
  EXPECT_TRUE(response.bodyInMemory().empty());
  EXPECT_EQ(response.headerValueOrEmpty(http::ETag), "\"version,1\"");
  EXPECT_EQ(etagCalls, 1U);
}

TEST_F(ResponseCacheIntegration, UncacheableRefreshInvalidatesOlderVariants) {
  HttpClient client;
  EXPECT_EQ(Value(client.get(Url("/vary-policy"))).bodyInMemory(), "vary-policy-1");

  auto refreshVary = client.makeRequest(http::Method::GET, Url("/vary-policy"));
  refreshVary.headerAddLine(http::CacheControl, "no-cache");
  EXPECT_EQ(Value(client.request(std::move(refreshVary))).bodyInMemory(), "vary-policy-2");
  EXPECT_EQ(Value(client.get(Url("/vary-policy"))).bodyInMemory(), "vary-policy-3");
  EXPECT_EQ(varyPolicyCalls, 3U);

  EXPECT_EQ(Value(client.get(Url("/set-cookie"))).bodyInMemory(), "set-cookie-1");
  auto refreshCookie = client.makeRequest(http::Method::GET, Url("/set-cookie"));
  refreshCookie.headerAddLine(http::CacheControl, "no-cache");
  EXPECT_EQ(Value(client.request(std::move(refreshCookie))).bodyInMemory(), "set-cookie-2");
  EXPECT_EQ(Value(client.get(Url("/set-cookie"))).bodyInMemory(), "set-cookie-3");
  EXPECT_EQ(setCookieCalls, 3U);
}

TEST_F(ResponseCacheIntegration, RequestNoCacheRefreshesTheStoredRepresentation) {
  HttpClient client;
  EXPECT_EQ(Value(client.get(Url("/refresh"))).bodyInMemory(), "refresh-1");
  EXPECT_EQ(Value(client.get(Url("/refresh"))).bodyInMemory(), "refresh-1");

  auto request = client.makeRequest(http::Method::GET, Url("/refresh"));
  request.headerAddLine(http::CacheControl, "no-cache");
  EXPECT_EQ(Value(client.request(std::move(request))).bodyInMemory(), "refresh-2");
  EXPECT_EQ(Value(client.get(Url("/refresh"))).bodyInMemory(), "refresh-2");
  EXPECT_EQ(refreshCalls, 2U);
}

TEST_F(ResponseCacheIntegration, ExpiresEntriesAtTheFreshnessDeadline) {
  HttpClient client;
  EXPECT_EQ(Value(client.get(Url("/expires"))).bodyInMemory(), "expires-1");
  EXPECT_EQ(Value(client.get(Url("/expires"))).bodyInMemory(), "expires-1");
  std::this_thread::sleep_for(1100ms);
  EXPECT_EQ(Value(client.get(Url("/expires"))).bodyInMemory(), "expires-2");
  EXPECT_EQ(expirationCalls, 2U);
  EXPECT_EQ(CacheStats("/expires").expirations, 1U);
}

TEST_F(ResponseCacheIntegration, DoesNotStoreForbiddenResponseDirectives) {
  HttpClient client;
  for (const std::string_view directive : {"no-store", "no-cache", "private", "max-age=0"}) {
    const std::string path = "/directive/" + std::string(directive);
    (void)Value(client.get(Url(path)));
    (void)Value(client.get(Url(path)));
  }
  EXPECT_EQ(directiveCalls, 8U);
}

TEST_F(ResponseCacheIntegration, BypassesSensitiveAndRangeRequestsByDefault) {
  HttpClient client;
  auto requestWith = [&](LowerAsciiKey name, std::string_view value) {
    auto request = client.makeRequest(http::Method::GET, Url("/sensitive"));
    request.headerAddLine(name, value);
    return Value(client.request(std::move(request)));
  };

  (void)requestWith(http::Authorization, "Bearer secret");
  (void)requestWith(http::Authorization, "Bearer secret");
  (void)requestWith(http::Cookie, "session=secret");
  (void)requestWith(http::Cookie, "session=secret");
  (void)requestWith(http::Range, "bytes=0-3");
  (void)requestWith(http::Range, "bytes=0-3");
  (void)Value(client.get(Url("/sensitive")));
  (void)Value(client.get(Url("/sensitive")));
  EXPECT_EQ(sensitiveCalls, 7U);
}

TEST_F(ResponseCacheIntegration, EnforcesPerRouteLruEntryLimit) {
  HttpClient client;
  EXPECT_EQ(Value(client.get(Url("/lru?item=a"))).bodyInMemory(), "a-1");
  EXPECT_EQ(Value(client.get(Url("/lru?item=b"))).bodyInMemory(), "b-1");
  EXPECT_EQ(Value(client.get(Url("/lru?item=a"))).bodyInMemory(), "a-2");
  EXPECT_EQ(lruACalls, 2U);
  EXPECT_EQ(lruBCalls, 1U);
  EXPECT_EQ(CacheStats("/lru").currentEntries, 1U);
  EXPECT_GE(CacheStats("/lru").evictions, 2U);
}

TEST_F(ResponseCacheIntegration, EnforcesMemoryBudgetAndSupportsExplicitInvalidation) {
  HttpClient client;
  (void)Value(client.get(Url("/memory?item=a")));
  (void)Value(client.get(Url("/memory?item=b")));
  (void)Value(client.get(Url("/memory?item=a")));
  EXPECT_EQ(memoryACalls, 2U);
  EXPECT_EQ(memoryBCalls, 1U);
  const auto memoryStats = CacheStats("/memory");
  EXPECT_LE(memoryStats.currentMemoryBytes, memoryCache.config().maxMemoryBytes);
  EXPECT_EQ(memoryStats.currentEntries, 1U) << "stores=" << memoryStats.stores << ", bypasses=" << memoryStats.bypasses;

  (void)Value(client.get(Url("/basic")));
  (void)Value(client.get(Url("/basic")));
  EXPECT_EQ(basicCalls, 1U);
  EXPECT_EQ(InvalidateCachePath("/basic", "/basic"), 1U);
  (void)Value(client.get(Url("/basic")));
  EXPECT_EQ(basicCalls, 2U);
  ClearCache("/basic");
  EXPECT_EQ(CacheStats("/basic").currentEntries, 0U);
  EXPECT_EQ(CacheStats("/basic").currentMemoryBytes, 0U);
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
TEST_F(ResponseCacheIntegration, CachesAsyncHandlerResponses) {
  HttpClient client;
  EXPECT_EQ(Value(client.get(Url("/async"))).bodyInMemory(), "async-1");
  EXPECT_EQ(Value(client.get(Url("/async"))).bodyInMemory(), "async-1");
  EXPECT_EQ(asyncCalls, 1U);
}
#endif

#ifdef AERONET_ENABLE_HTTP2
TEST_F(ResponseCacheIntegration, UsesTheSameLifecycleOverHttp2) {
  HttpClientConfig config;
  config.withHttpVersion(HttpVersionMode::Http2);
  HttpClient client(config);
  EXPECT_EQ(Value(client.get(Url("/h2"))).bodyInMemory(), "h2-1");
  EXPECT_EQ(Value(client.get(Url("/h2"))).bodyInMemory(), "h2-1");
  EXPECT_EQ(h2Calls, 1U);
}
#endif

TEST(ResponseCacheConfiguration, RejectsInvalidBudgetsAndMethods) {
  ResponseCacheConfig config;
  config.maxEntries = 0;
  EXPECT_THROW((void)ResponseCache(config), std::invalid_argument);

  config = {};
  config.maxEntryBytes = config.maxMemoryBytes + 1U;
  EXPECT_THROW((void)ResponseCache(config), std::invalid_argument);

  config = {};
  config.methods = static_cast<http::MethodBmp>(http::Method::POST);
  EXPECT_THROW((void)ResponseCache(config), std::invalid_argument);
}

TEST(ResponseCacheRouteApi, RouterAndGroupCopiesOwnIndependentCaches) {
  ResponseCache routeCache;
  Router router;
  auto api = router.group("/api").withResponseCache(routeCache);
  api.setPath(http::Method::GET, "/one", [](const HttpRequestView&) { return HttpResponse{}; });
  auto nested = api.group("/v2");
  nested.setPath(http::Method::GET, "/two", [](const HttpRequestView&) { return HttpResponse{}; });

  ResponseCache* const first = router.match(http::Method::GET, "/api/one").responseCache();
  ResponseCache* const second = router.match(http::Method::GET, "/api/v2/two").responseCache();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first, &routeCache);
  EXPECT_NE(second, &routeCache);
  EXPECT_NE(first, second);

  Router copied = router;
  ResponseCache* const copiedFirst = copied.match(http::Method::GET, "/api/one").responseCache();
  ASSERT_NE(copiedFirst, nullptr);
  EXPECT_NE(copiedFirst, first);
  EXPECT_EQ(copiedFirst->config().maxEntries, routeCache.config().maxEntries);
  EXPECT_EQ(copiedFirst->stats().currentEntries, 0U);
}

#endif  // AERONET_ENABLE_RESPONSE_CACHE

}  // namespace
}  // namespace aeronet
