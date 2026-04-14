#include "aeronet/path-handler-entry.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/path-handlers.hpp"
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/request-task.hpp"
#endif
#include "aeronet/router.hpp"

#ifdef AERONET_WINDOWS
// Windows compatibility: <winnt.h> (pulled in by GoogleTest on Windows) defines DELETE as
// a security-access-mask macro (0x00010000L) after http-method.hpp has already #undef'd it once.
// Undefine it again here so that http::Method::DELETE refers to the enum member, not the macro.
#ifdef DELETE
#undef DELETE
#endif
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
#include "aeronet/websocket-endpoint.hpp"
#endif

namespace aeronet {

class HttpResponseWriter;

namespace {

RequestHandler MakeNormalHandler() {
  return [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); };
}

StreamingHandler MakeStreamingHandler() {
  return [](const HttpRequest&, HttpResponseWriter&) {};
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
AsyncRequestHandler MakeAsyncHandler() {
  return [](HttpRequest&) -> RequestTask<HttpResponse> { co_return HttpResponse(http::StatusCodeOK); };
}
#endif

}  // namespace

class PathHandlerEntryTest : public ::testing::Test {
 protected:
  Router router;
  PathHandlerEntry entry;

  void assignNormal(http::MethodBmp bmp, RequestHandler handler) { entry.assignNormalHandler(bmp, std::move(handler)); }

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  void assignAsync(http::MethodBmp bmp, AsyncRequestHandler handler) {
    entry.assignAsyncHandler(bmp, std::move(handler));
  }
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
  static void assignWebSocketEndpoint(PathHandlerEntry& entry) {
    entry.assignWebSocketEndpoint(WebSocketEndpoint::WithCallbacks({
        .onMessage = {},
        .onPing = {},
        .onPong = {},
        .onClose = {},
        .onError = {},
    }));
  }
#endif

  void addPaths() {
    router.setPath(http::Method::GET, "/ctor", MakeNormalHandler());
    router.setPath(http::Method::GET, "/ctor", MakeNormalHandler());  // should override previous
    router.setPath(http::Method::POST, "/ctor", MakeStreamingHandler());
    router.setPath(http::Method::POST, "/ctor", MakeStreamingHandler());  // should override previous
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    router.setPath(http::Method::PUT, "/ctor", MakeAsyncHandler());
    entry = router.setPath(http::Method::PUT, "/ctor", MakeAsyncHandler());  // should override previous
#endif
  }

  static auto hasAnyHandler(const PathHandlerEntry& entry) { return entry.hasAnyHandler(); }

  static auto getCorsPolicy(const PathHandlerEntry& entry) { return entry._corsPolicy; }
  static auto getPreMiddleware(const PathHandlerEntry& entry) { return entry._preMiddleware; }
  static auto getPostMiddleware(const PathHandlerEntry& entry) { return entry._postMiddleware; }
  static auto getNormalMethodBmp(const PathHandlerEntry& entry) { return entry._normalMethodBmp; }
  static auto getStreamingMethodBmp(const PathHandlerEntry& entry) { return entry._streamingMethodBmp; }
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  static auto getAsyncMethodBmp(const PathHandlerEntry& entry) { return entry._asyncMethodBmp; }
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
  static bool hasWebSocketEndpoint(const PathHandlerEntry& entry) { return entry.hasWebSocketEndpoint(); }
  static auto* webSocketEndpointPtr(const PathHandlerEntry& entry) { return entry.webSocketEndpointPtr(); }
#endif

  static auto* normalHandlerPtr(const PathHandlerEntry& entry, http::MethodIdx idx) {
    return entry.requestHandlerPtr(idx);
  }
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  static auto* asyncHandlerPtr(const PathHandlerEntry& entry, http::MethodIdx idx) {
    return entry.asyncHandlerPtr(idx);
  }
#endif
};

TEST_F(PathHandlerEntryTest, DefaultConstructor) { EXPECT_FALSE(hasAnyHandler(entry)); }

TEST_F(PathHandlerEntryTest, SetPathEmpty) {
  EXPECT_THROW(router.setPath(http::Method::GET, "/", RequestHandler{}), std::invalid_argument);
  EXPECT_THROW(router.setPath(http::Method::GET, "/", StreamingHandler{}), std::invalid_argument);
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  EXPECT_THROW(router.setPath(http::Method::GET, "/", AsyncRequestHandler{}), std ::invalid_argument);
#endif
}

TEST_F(PathHandlerEntryTest, SpecialOperationsWithoutWebSocket) {
  entry = router.setPath(http::Method::GET, "/ctor", MakeNormalHandler());
  addPaths();
  entry.before([](HttpRequest&) { return MiddlewareResult::Continue(); })
      .after([](const HttpRequest&, HttpResponse&) {})
      .cors(CorsPolicy(CorsPolicy::Active::On).allowAnyOrigin());

  PathHandlerEntry copied(entry);
  EXPECT_EQ(getNormalMethodBmp(copied), getNormalMethodBmp(entry));
#ifdef AERONET_ENABLE_WEBSOCKET
  EXPECT_EQ(hasWebSocketEndpoint(copied), hasWebSocketEndpoint(entry));
#endif
  entry.after({});
  EXPECT_NE(getPostMiddleware(copied).size(), getPostMiddleware(entry).size());
  entry = copied;
  EXPECT_EQ(getPostMiddleware(copied).size(), getPostMiddleware(entry).size());
  entry.after({});

  PathHandlerEntry moved(std::move(entry));
  EXPECT_EQ(getNormalMethodBmp(moved), getNormalMethodBmp(copied));
  entry = std::move(moved);
  EXPECT_EQ(getNormalMethodBmp(entry), getNormalMethodBmp(copied));
  moved = router.setPath(http::Method::GET, "/ctor", MakeNormalHandler());
  entry = std::move(moved);

  auto& alias = entry;
  entry = alias;  // should do nothing
  entry = std::move(alias);
}

#ifdef AERONET_ENABLE_WEBSOCKET
TEST_F(PathHandlerEntryTest, SpecialOperationsWithWebSocket) {
  entry = router.setPath(http::Method::GET, "/ctor", MakeNormalHandler());
  addPaths();
  entry.before([](HttpRequest&) { return MiddlewareResult::Continue(); })
      .after([](const HttpRequest&, HttpResponse&) {})
      .cors(CorsPolicy(CorsPolicy::Active::On).allowAnyOrigin());

  assignWebSocketEndpoint(entry);
  assignWebSocketEndpoint(entry);

  PathHandlerEntry copied(entry);
  EXPECT_EQ(getNormalMethodBmp(copied), getNormalMethodBmp(entry));
  EXPECT_EQ(hasWebSocketEndpoint(copied), hasWebSocketEndpoint(entry));
  entry.after({});
  EXPECT_NE(getPostMiddleware(copied).size(), getPostMiddleware(entry).size());
  entry = copied;
  EXPECT_EQ(getPostMiddleware(copied).size(), getPostMiddleware(entry).size());
  entry.after({});

  PathHandlerEntry moved(std::move(entry));
  EXPECT_EQ(getNormalMethodBmp(moved), getNormalMethodBmp(copied));
  entry = std::move(moved);
  EXPECT_EQ(getNormalMethodBmp(entry), getNormalMethodBmp(copied));
}
#endif

TEST_F(PathHandlerEntryTest, CopyAndMoveConstructorsCoverMixedHandlers) {
  addPaths();
  router.setPath(http::Method::GET, "/ctor", MakeNormalHandler())
      .before([](HttpRequest&) { return MiddlewareResult::Continue(); })
      .after([](const HttpRequest&, HttpResponse&) {})
      .cors(CorsPolicy(CorsPolicy::Active::On).allowAnyOrigin());

  auto result = router.match(http::Method::GET, "/ctor");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Request);
  EXPECT_NE(result.requestHandler(), nullptr);
  EXPECT_EQ(result.preMiddlewareRange().size(), 1U);

  result.resetHandler();
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::None);
  EXPECT_FALSE(result.hasHandler());
  result = router.match(http::Method::POST, "/ctor");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Streaming);
  EXPECT_NE(result.streamingHandler(), nullptr);
  EXPECT_EQ(result.postMiddlewareRange().size(), 1U);
}

TEST_F(PathHandlerEntryTest, CopyAssignmentTransfersNormalHandlers) {
  auto& sourceEntry = router.setPath(http::Method::GET, "/copy-src", MakeNormalHandler());
  Router target;
  auto& targetEntry = target.setPath(http::Method::DELETE, "/copy-dst", MakeNormalHandler());

  targetEntry = sourceEntry;
  auto result = target.match(http::Method::GET, "/copy-dst");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Request);
  EXPECT_NE(result.requestHandler(), nullptr);
  EXPECT_TRUE(result.hasHandler());
}

TEST_F(PathHandlerEntryTest, CopyAssignmentReusesExistingStreamingStorage) {
  auto& sourceEntry = router.setPath(http::Method::POST, "/stream-src", MakeStreamingHandler());
  Router target;
  auto& targetEntry = target.setPath(http::Method::POST, "/stream-target", MakeStreamingHandler());

  targetEntry = sourceEntry;
  auto result = target.match(http::Method::POST, "/stream-target");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Streaming);
  EXPECT_TRUE(result.hasHandler());
}

TEST_F(PathHandlerEntryTest, CopyAssignmentConstructsNewStreamingHandler) {
  auto& sourceEntry = router.setPath(http::Method::POST, "/stream-src-2", MakeStreamingHandler());
  Router target;
  auto& targetEntry = target.setPath(http::Method::DELETE, "/stream-target-2", MakeStreamingHandler());

  targetEntry = sourceEntry;
  auto result = target.match(http::Method::POST, "/stream-target-2");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Streaming);
  EXPECT_TRUE(result.hasHandler());
}

TEST_F(PathHandlerEntryTest, MoveAssignmentTransfersStreamingHandlers) {
  auto& sourceEntry = router.setPath(http::Method::POST, "/move-stream-src", MakeStreamingHandler());
  Router target;
  auto& targetEntry = target.setPath(http::Method::POST, "/move-stream-target", MakeStreamingHandler());

  targetEntry = std::move(sourceEntry);
  auto result = target.match(http::Method::POST, "/move-stream-target");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Streaming);
}

TEST_F(PathHandlerEntryTest, SeveralStreamingAssignments) {
  router.setPath(http::Method::GET | http::Method::POST | http::Method::PUT, "/streaming", MakeStreamingHandler());

  EXPECT_NE(router.match(http::Method::GET, "/streaming").streamingHandler(), nullptr);
  EXPECT_EQ(router.match(http::Method::PATCH, "/streaming").streamingHandler(), nullptr);

  router.setPath(http::Method::POST | http::Method::PUT | http::Method::PATCH | http::Method::HEAD, "/streaming",
                 MakeStreamingHandler());
  EXPECT_NE(router.match(http::Method::PATCH, "/streaming").streamingHandler(), nullptr);
  EXPECT_NE(router.match(http::Method::GET, "/streaming").streamingHandler(), nullptr);

  EXPECT_NE(router.match(http::Method::PUT, "/streaming").streamingHandler(), nullptr);
  EXPECT_EQ(router.match(http::Method::CONNECT, "/streaming").streamingHandler(), nullptr);

  EXPECT_EQ(router.match(http::Method::GET, "/streaming2").streamingHandler(), nullptr);
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS

TEST_F(PathHandlerEntryTest, CopyAssignmentConstructsAsyncHandler) {
  auto& sourceEntry = router.setPath(http::Method::PUT, "/async-src", MakeAsyncHandler());
  Router target;
  auto& targetEntry = target.setPath(http::Method::DELETE, "/async-target", MakeNormalHandler());

  targetEntry = sourceEntry;
  auto result = target.match(http::Method::PUT, "/async-target");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Async);
  EXPECT_TRUE(result.hasHandler());
}

TEST_F(PathHandlerEntryTest, MoveAssignmentConstructsNewAsyncHandler) {
  auto& sourceEntry = router.setPath(http::Method::PATCH, "/move-async-src", MakeAsyncHandler());
  Router target;
  auto& targetEntry = target.setPath(http::Method::DELETE, "/move-async-target", MakeNormalHandler());

  targetEntry = std::move(sourceEntry);
  auto result = target.match(http::Method::PATCH, "/move-async-target");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Async);
}

#endif

TEST_F(PathHandlerEntryTest, CorsAndMiddlewarePopulatedOnMatch) {
  router.setPath(http::Method::GET, "/middleware", MakeNormalHandler())
      .cors(CorsPolicy(CorsPolicy::Active::On).allowAnyOrigin())
      .before([](HttpRequest&) { return MiddlewareResult::Continue(); })
      .after([](const HttpRequest&, HttpResponse&) {});

  auto result = router.match(http::Method::GET, "/middleware");
  ASSERT_NE(result.pCorsPolicy, nullptr);
  EXPECT_TRUE(result.pCorsPolicy->active());
  EXPECT_EQ(result.preMiddlewareRange().size(), 1U);
  EXPECT_EQ(result.postMiddlewareRange().size(), 1U);
}

TEST_F(PathHandlerEntryTest, NormalAfterStreamingThrows) {
  router.setPath(http::Method::GET, "/conflict", MakeStreamingHandler());
  EXPECT_THROW(router.setPath(http::Method::GET, "/conflict", MakeNormalHandler()), std::logic_error);
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS

TEST_F(PathHandlerEntryTest, NormalAfterAsyncThrows) {
  router.setPath(http::Method::GET, "/conflict-async", MakeAsyncHandler());
  EXPECT_THROW(router.setPath(http::Method::GET, "/conflict-async", MakeNormalHandler()), std::logic_error);
}

TEST_F(PathHandlerEntryTest, AsyncAfterNormalThrows) {
  router.setPath(http::Method::GET, "/conflict-async-2", MakeNormalHandler());
  EXPECT_THROW(router.setPath(http::Method::GET, "/conflict-async-2", MakeAsyncHandler()), std::logic_error);
}

TEST_F(PathHandlerEntryTest, AsyncAfterStreamingThrows) {
  router.setPath(http::Method::GET, "/conflict-async-3", MakeStreamingHandler());
  EXPECT_THROW(router.setPath(http::Method::GET, "/conflict-async-3", MakeAsyncHandler()), std::logic_error);
}

#endif

TEST_F(PathHandlerEntryTest, StreamingAfterNormalThrows) {
  router.setPath(http::Method::GET, "/conflict-stream-1", MakeNormalHandler());
  EXPECT_THROW(router.setPath(http::Method::GET, "/conflict-stream-1", MakeStreamingHandler()), std::logic_error);
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS

TEST_F(PathHandlerEntryTest, StreamingAfterAsyncThrows) {
  router.setPath(http::Method::GET, "/conflict-stream-2", MakeAsyncHandler());
  EXPECT_THROW(router.setPath(http::Method::GET, "/conflict-stream-2", MakeStreamingHandler()), std::logic_error);
}

#endif

TEST_F(PathHandlerEntryTest, AssignNormalHandlerCopiesWithinSingleCall) {
  // Assign one handler to two methods in a single call so the implementation will
  // construct the handler for the first method and copy it for the second (pLastAsyncHandler reuse).
  assignNormal(http::Method::GET | http::Method::POST, MakeNormalHandler());
  assignNormal(http::Method::GET | http::Method::CONNECT | http::Method::HEAD | http::Method::POST,
               MakeNormalHandler());

  const RequestHandler* p0 = normalHandlerPtr(entry, 0);
  const RequestHandler* p1 = normalHandlerPtr(entry, 1);

  ASSERT_NE(p0, nullptr);
  ASSERT_NE(p1, nullptr);
  // Distinct storage slots are constructed from the same handler instance.
  EXPECT_NE(p0, p1);
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS

TEST_F(PathHandlerEntryTest, AssignAsyncHandlerCopiesWithinSingleCall) {
  // Assign one handler to two methods in a single call so the implementation will
  // construct the handler for the first method and copy it for the second (pLastAsyncHandler reuse).
  assignAsync(http::Method::GET | http::Method::POST, MakeAsyncHandler());
  assignAsync(http::Method::GET | http::Method::CONNECT | http::Method::HEAD | http::Method::POST, MakeAsyncHandler());

  const AsyncRequestHandler* p0 = asyncHandlerPtr(entry, 0);
  const AsyncRequestHandler* p1 = asyncHandlerPtr(entry, 1);

  ASSERT_NE(p0, nullptr);
  ASSERT_NE(p1, nullptr);
  // Distinct storage slots are constructed from the same handler instance.
  EXPECT_NE(p0, p1);
}

#endif

TEST_F(PathHandlerEntryTest, PathEntryConfigDefaultValues) {
  PathEntryConfig config;
#ifdef AERONET_ENABLE_HTTP2
  EXPECT_EQ(config.http2Enable, PathEntryConfig::Http2Enable::Default);
#endif
  EXPECT_EQ(config.requestTimeout, std::chrono::milliseconds::max());
  EXPECT_EQ(config.maxBodyBytes, static_cast<std::size_t>(-1));
  EXPECT_EQ(config.maxHeaderBytes, static_cast<uint32_t>(-1));
}

TEST_F(PathHandlerEntryTest, PathEntryConfigDesignatedInitializers) {
  using namespace std::chrono_literals;
  PathEntryConfig config{.maxHeaderBytes = 512, .maxBodyBytes = 1024, .requestTimeout = 5000ms};
  EXPECT_EQ(config.requestTimeout, 5000ms);
  EXPECT_EQ(config.maxBodyBytes, 1024U);
  EXPECT_EQ(config.maxHeaderBytes, 512U);
}

TEST_F(PathHandlerEntryTest, ChainableTimeoutSetter) {
  using namespace std::chrono_literals;
  auto& ref = router.setPath(http::Method::POST, "/timeout-test", MakeNormalHandler()).timeout(3000ms);
  auto result = router.match(http::Method::POST, "/timeout-test");
  EXPECT_EQ(result.pathConfig.requestTimeout, 3000ms);
  static_assert(std::is_same_v<decltype(ref), PathHandlerEntry&>);
}

TEST_F(PathHandlerEntryTest, ExcessiveTimeoutThrows) {
  using namespace std::chrono_literals;
  EXPECT_THROW(router.setPath(http::Method::GET, "/too-long", MakeNormalHandler()).timeout(std::chrono::hours{24 * 50}),
               std::invalid_argument);
}

TEST_F(PathHandlerEntryTest, MaxMillisecondsTimeoutDoesNotThrow) {
  // milliseconds::max() is the sentinel for "no per-route timeout" — must not be rejected.
  EXPECT_NO_THROW(
      router.setPath(http::Method::GET, "/max-ok", MakeNormalHandler()).timeout(std::chrono::milliseconds::max()));
}

TEST_F(PathHandlerEntryTest, ChainableMaxBodyBytesSetter) {
  auto& ref = router.setPath(http::Method::PUT, "/body-limit", MakeNormalHandler()).maxBodyBytes(65536);
  auto result = router.match(http::Method::PUT, "/body-limit");
  EXPECT_EQ(result.pathConfig.maxBodyBytes, 65536U);
  static_assert(std::is_same_v<decltype(ref), PathHandlerEntry&>);
}

TEST_F(PathHandlerEntryTest, ChainableMaxHeaderBytesSetter) {
  auto& ref = router.setPath(http::Method::GET, "/header-limit", MakeNormalHandler()).maxHeaderBytes(2048);
  auto result = router.match(http::Method::GET, "/header-limit");
  EXPECT_EQ(result.pathConfig.maxHeaderBytes, 2048U);
  static_assert(std::is_same_v<decltype(ref), PathHandlerEntry&>);
}

TEST_F(PathHandlerEntryTest, ChainingMultipleSetters) {
  using namespace std::chrono_literals;
  router.setPath(http::Method::POST, "/all-config", MakeNormalHandler())
      .timeout(1000ms)
      .maxBodyBytes(4096)
      .maxHeaderBytes(1024)
#ifdef AERONET_ENABLE_HTTP2
      .http2Enable(PathEntryConfig::Http2Enable::Disable)
#endif
      .cors(CorsPolicy(CorsPolicy::Active::On).allowAnyOrigin());

  auto result = router.match(http::Method::POST, "/all-config");
  EXPECT_EQ(result.pathConfig.requestTimeout, 1000ms);
  EXPECT_EQ(result.pathConfig.maxBodyBytes, 4096U);
  EXPECT_EQ(result.pathConfig.maxHeaderBytes, 1024U);
#ifdef AERONET_ENABLE_HTTP2
  EXPECT_EQ(result.pathConfig.http2Enable, PathEntryConfig::Http2Enable::Disable);
#endif
  ASSERT_NE(result.pCorsPolicy, nullptr);
  EXPECT_TRUE(result.pCorsPolicy->active());
}

TEST_F(PathHandlerEntryTest, RouteWithoutOverridesHasNulloptConfig) {
  router.setPath(http::Method::GET, "/no-config", MakeNormalHandler());
  auto result = router.match(http::Method::GET, "/no-config");
  EXPECT_EQ(result.pathConfig.requestTimeout, std::chrono::milliseconds::max());
  EXPECT_EQ(result.pathConfig.maxBodyBytes, static_cast<std::size_t>(-1));
  EXPECT_EQ(result.pathConfig.maxHeaderBytes, static_cast<uint32_t>(-1));
#ifdef AERONET_ENABLE_HTTP2
  EXPECT_EQ(result.pathConfig.http2Enable, PathEntryConfig::Http2Enable::Default);
#endif
}

TEST_F(PathHandlerEntryTest, CopyPreservesNewPathConfigFields) {
  using namespace std::chrono_literals;
  auto& sourceEntry =
      router.setPath(http::Method::GET, "/copy-cfg-src", MakeNormalHandler()).timeout(2000ms).maxBodyBytes(8192);

  Router target;
  auto& targetEntry = target.setPath(http::Method::DELETE, "/copy-cfg-dst", MakeNormalHandler());
  targetEntry = sourceEntry;

  auto result = target.match(http::Method::GET, "/copy-cfg-dst");
  EXPECT_EQ(result.pathConfig.requestTimeout, 2000ms);
  EXPECT_EQ(result.pathConfig.maxBodyBytes, 8192U);
}

TEST_F(PathHandlerEntryTest, MovePreservesNewPathConfigFields) {
  using namespace std::chrono_literals;
  auto& sourceEntry =
      router.setPath(http::Method::GET, "/move-cfg-src", MakeNormalHandler()).timeout(500ms).maxHeaderBytes(4096);

  Router target;
  auto& targetEntry = target.setPath(http::Method::DELETE, "/move-cfg-dst", MakeNormalHandler());
  targetEntry = std::move(sourceEntry);

  auto result = target.match(http::Method::GET, "/move-cfg-dst");
  EXPECT_EQ(result.pathConfig.requestTimeout, 500ms);
  EXPECT_EQ(result.pathConfig.maxHeaderBytes, 4096U);
}

}  // namespace aeronet