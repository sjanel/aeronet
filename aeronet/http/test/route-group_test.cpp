#include "aeronet/route-group.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/path-handler-entry.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/router.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/request-task.hpp"
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
#include "aeronet/websocket-endpoint.hpp"
#include "aeronet/websocket-handler.hpp"
#endif

namespace aeronet {

class HttpResponseWriter;

namespace {

RequestHandler MakeHandler() {
  return [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); };
}

StreamingHandler MakeStreamingHandler() {
  return [](const HttpRequest&, HttpResponseWriter&) {};
}

}  // namespace

class RouteGroupTest : public ::testing::Test {
 protected:
  Router router;
};

TEST_F(RouteGroupTest, PrefixConcatenation) {
  auto api = router.group("/api/v1");
  api.setPath(http::Method::GET, "/users", MakeHandler());

  auto result = router.match(http::Method::GET, "/api/v1/users");
  EXPECT_NE(result.requestHandler(), nullptr);
}

TEST_F(RouteGroupTest, GroupAppliesSharedCors) {
  auto api = router.group("/api");
  api.withCors(CorsPolicy(CorsPolicy::Active::On).allowAnyOrigin());
  api.setPath(http::Method::GET, "/items", MakeHandler());

  auto result = router.match(http::Method::GET, "/api/items");
  ASSERT_NE(result.pCorsPolicy, nullptr);
  EXPECT_TRUE(result.pCorsPolicy->active());
}

TEST_F(RouteGroupTest, GroupAppliesSharedMiddleware) {
  auto api = router.group("/mw");
  int preCalled = 0;
  int postCalled = 0;
  api.addRequestMiddleware([&preCalled](HttpRequest&) {
    ++preCalled;
    return MiddlewareResult::Continue();
  });
  api.addResponseMiddleware([&postCalled](const HttpRequest&, HttpResponse&) { ++postCalled; });
  api.setPath(http::Method::GET, "/a", MakeHandler());
  api.setPath(http::Method::GET, "/b", MakeHandler());

  auto ra = router.match(http::Method::GET, "/mw/a");
  auto rb = router.match(http::Method::GET, "/mw/b");
  EXPECT_EQ(ra.preMiddlewareRange().size(), 1U);
  EXPECT_EQ(rb.preMiddlewareRange().size(), 1U);
  EXPECT_EQ(ra.postMiddlewareRange().size(), 1U);
  EXPECT_EQ(rb.postMiddlewareRange().size(), 1U);
}

TEST_F(RouteGroupTest, GroupAppliesMaxBodyBytes) {
  auto api = router.group("/upload");
  api.withMaxBodyBytes(1024);
  api.setPath(http::Method::POST, "/file", MakeHandler());

  auto result = router.match(http::Method::POST, "/upload/file");
  EXPECT_EQ(result.pathConfig.maxBodyBytes, 1024U);
}

TEST_F(RouteGroupTest, GroupAppliesTimeout) {
  using namespace std::chrono_literals;
  auto api = router.group("/slow");
  api.withTimeout(5000ms);
  api.setPath(http::Method::GET, "/report", MakeHandler());

  auto result = router.match(http::Method::GET, "/slow/report");
  EXPECT_EQ(result.pathConfig.requestTimeout, 5000ms);
}

TEST_F(RouteGroupTest, GroupAppliesMaxHeaderBytes) {
  auto api = router.group("/strict");
  api.withMaxHeaderBytes(2048);
  api.setPath(http::Method::GET, "/data", MakeHandler());

  auto result = router.match(http::Method::GET, "/strict/data");
  EXPECT_EQ(result.pathConfig.maxHeaderBytes, 2048U);
}

#ifdef AERONET_ENABLE_HTTP2
TEST_F(RouteGroupTest, GroupAppliesHttp2Enable) {
  auto api = router.group("/h2");
  api.withHttp2Enable(PathEntryConfig::Http2Enable::Disable);
  api.setPath(http::Method::GET, "/legacy", MakeHandler());

  auto result = router.match(http::Method::GET, "/h2/legacy");
  EXPECT_EQ(result.pathConfig.http2Enable, PathEntryConfig::Http2Enable::Disable);
}
#endif

TEST_F(RouteGroupTest, PerRouteOverrideWinsOverGroup) {
  auto api = router.group("/override");
  api.withMaxBodyBytes(4096);
  api.setPath(http::Method::POST, "/large", MakeHandler()).maxBodyBytes(65536);

  auto result = router.match(http::Method::POST, "/override/large");
  // Per-route setter applied after group default → overrides the group value
  EXPECT_EQ(result.pathConfig.maxBodyBytes, 65536U);
}

TEST_F(RouteGroupTest, NestedGroupConcatenatesPrefix) {
  auto api = router.group("/api");
  auto v2 = api.group("/v2");
  v2.setPath(http::Method::GET, "/items", MakeHandler());

  auto result = router.match(http::Method::GET, "/api/v2/items");
  EXPECT_NE(result.requestHandler(), nullptr);
}

TEST_F(RouteGroupTest, NestedGroupInheritsConfig) {
  using namespace std::chrono_literals;
  auto api = router.group("/api");
  api.withTimeout(3000ms).withMaxBodyBytes(8192);
  auto v2 = api.group("/v2");
  v2.setPath(http::Method::POST, "/data", MakeHandler());

  auto result = router.match(http::Method::POST, "/api/v2/data");
  EXPECT_EQ(result.pathConfig.requestTimeout, 3000ms);
  EXPECT_EQ(result.pathConfig.maxBodyBytes, 8192U);
}

TEST_F(RouteGroupTest, NestedGroupInheritsMiddleware) {
  auto api = router.group("/api");
  api.addRequestMiddleware([](HttpRequest&) { return MiddlewareResult::Continue(); });
  auto v2 = api.group("/v2");
  v2.addRequestMiddleware([](HttpRequest&) { return MiddlewareResult::Continue(); });
  v2.setPath(http::Method::GET, "/info", MakeHandler());

  auto result = router.match(http::Method::GET, "/api/v2/info");
  // Inherited parent middleware + child middleware
  EXPECT_EQ(result.preMiddlewareRange().size(), 2U);
}

TEST_F(RouteGroupTest, NestedGroupCanOverrideParentConfig) {
  auto api = router.group("/api");
  api.withMaxBodyBytes(4096);
  auto uploads = api.group("/uploads");
  uploads.withMaxBodyBytes(1048576);  // Override for upload sub-group
  uploads.setPath(http::Method::POST, "/photo", MakeHandler());

  auto result = router.match(http::Method::POST, "/api/uploads/photo");
  EXPECT_EQ(result.pathConfig.maxBodyBytes, 1048576U);
}

TEST_F(RouteGroupTest, StreamingHandlerViaBmpGroup) {
  auto api = router.group("/stream");
  api.setPath(http::Method::GET | http::Method::POST, "/data", MakeStreamingHandler());

  auto rGet = router.match(http::Method::GET, "/stream/data");
  EXPECT_NE(rGet.streamingHandler(), nullptr);
  auto rPost = router.match(http::Method::POST, "/stream/data");
  EXPECT_NE(rPost.streamingHandler(), nullptr);
}

TEST_F(RouteGroupTest, MultipleRoutesInSameGroup) {
  auto api = router.group("/multi");
  api.withMaxBodyBytes(2048);
  api.setPath(http::Method::GET, "/a", MakeHandler());
  api.setPath(http::Method::GET, "/b", MakeHandler());
  api.setPath(http::Method::POST, "/c", MakeHandler());

  for (auto path : {"/multi/a", "/multi/b", "/multi/c"}) {
    auto result = router.match(http::Method::GET, path);
    if (result.requestHandler() != nullptr) {
      EXPECT_EQ(result.pathConfig.maxBodyBytes, 2048U) << "Path " << path << " missing maxBodyBytes";
    }
  }
}

TEST_F(RouteGroupTest, ChainableGroupConfiguration) {
  using namespace std::chrono_literals;
  auto api =
      router.group("/chain").withTimeout(2000ms).withMaxBodyBytes(1024).withMaxHeaderBytes(512).withCors(CorsPolicy{});
  api.setPath(http::Method::GET, "/test", MakeHandler());

  auto result = router.match(http::Method::GET, "/chain/test");
  EXPECT_EQ(result.pathConfig.requestTimeout, 2000ms);
  EXPECT_EQ(result.pathConfig.maxBodyBytes, 1024U);
  EXPECT_EQ(result.pathConfig.maxHeaderBytes, 512U);
}

TEST_F(RouteGroupTest, StreamingHandlerSingleMethod) {
  auto api = router.group("/s");
  api.setPath(http::Method::POST, "/upload", MakeStreamingHandler());

  auto result = router.match(http::Method::POST, "/s/upload");
  EXPECT_NE(result.streamingHandler(), nullptr);
  EXPECT_EQ(result.requestHandler(), nullptr);
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
TEST_F(RouteGroupTest, AsyncHandlerViaBmpGroup) {
  auto api = router.group("/async");
  api.withMaxBodyBytes(2048);
  api.setPath(http::Method::GET | http::Method::POST, "/task",
              [](HttpRequest&) -> RequestTask<HttpResponse> { co_return HttpResponse(http::StatusCodeOK); });

  auto rGet = router.match(http::Method::GET, "/async/task");
  EXPECT_NE(rGet.asyncRequestHandler(), nullptr);
  EXPECT_EQ(rGet.pathConfig.maxBodyBytes, 2048U);

  auto rPost = router.match(http::Method::POST, "/async/task");
  EXPECT_NE(rPost.asyncRequestHandler(), nullptr);
  EXPECT_EQ(rPost.pathConfig.maxBodyBytes, 2048U);
}

TEST_F(RouteGroupTest, AsyncHandlerSingleMethod) {
  auto api = router.group("/async2");
  api.setPath(http::Method::PUT, "/item",
              [](HttpRequest&) -> RequestTask<HttpResponse> { co_return HttpResponse(http::StatusCodeOK); });

  auto result = router.match(http::Method::PUT, "/async2/item");
  EXPECT_NE(result.asyncRequestHandler(), nullptr);
}
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
TEST_F(RouteGroupTest, WebSocketHandlerRegistration) {
  auto api = router.group("/ws-api");
  api.withMaxHeaderBytes(512);
  api.setWebSocket("/chat", WebSocketEndpoint::WithCallbacks(websocket::WebSocketCallbacks{}));

  auto result = router.match(http::Method::GET, "/ws-api/chat");
  EXPECT_NE(result.pWebSocketEndpoint, nullptr);
  EXPECT_EQ(result.pathConfig.maxHeaderBytes, 512U);
}
#endif

TEST_F(RouteGroupTest, ExcessiveTimeoutThrows) {
  auto api = router.group("/too-slow");
  EXPECT_THROW(api.withTimeout(std::chrono::hours{24 * 50}), std::invalid_argument);
}

TEST_F(RouteGroupTest, MaxMillisecondsTimeoutDoesNotThrow) {
  auto api = router.group("/sentinel");
  EXPECT_NO_THROW(api.withTimeout(std::chrono::milliseconds::max()));
}

}  // namespace aeronet
