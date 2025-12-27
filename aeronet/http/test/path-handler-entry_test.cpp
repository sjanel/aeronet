#include "aeronet/path-handler-entry.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <utility>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/request-task.hpp"
#include "aeronet/router.hpp"

namespace aeronet {

class HttpResponseWriter;

namespace {

RequestHandler MakeNormalHandler() {
  return [data = RawChars("some data 1")](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); };
}

StreamingHandler MakeStreamingHandler() {
  return [data = RawChars("some data 12")](const HttpRequest&, HttpResponseWriter&) {};
}

AsyncRequestHandler MakeAsyncHandler() {
  return [data = RawChars("some data 123")](HttpRequest&) -> RequestTask<HttpResponse> {
    co_return HttpResponse(http::StatusCodeOK);
  };
}

}  // namespace

class PathHandlerEntryTest : public ::testing::Test {
 protected:
  Router router;
  PathHandlerEntry entry;

  void assignNormal(http::MethodBmp bmp, RequestHandler handler) { entry.assignNormalHandler(bmp, std::move(handler)); }

  void assignAsync(http::MethodBmp bmp, AsyncRequestHandler handler) {
    entry.assignAsyncHandler(bmp, std::move(handler));
  }

  static void assignWebSocketEndpoint(PathHandlerEntry& entry) {
    entry.assignWebSocketEndpoint(WebSocketEndpoint::WithCallbacks({
        .onMessage = {},
        .onPing = {},
        .onPong = {},
        .onClose = {},
        .onError = {},
    }));
  }

  void addPaths() {
    router.setPath(http::Method::GET, "/ctor", MakeNormalHandler());
    router.setPath(http::Method::GET, "/ctor", MakeNormalHandler());  // should override previous
    router.setPath(http::Method::POST, "/ctor", MakeStreamingHandler());
    router.setPath(http::Method::POST, "/ctor", MakeStreamingHandler());  // should override previous
    router.setPath(http::Method::PUT, "/ctor", MakeAsyncHandler());
    entry = router.setPath(http::Method::PUT, "/ctor", MakeAsyncHandler());  // should override previous
  }

  static auto getCorsPolicy(const PathHandlerEntry& entry) { return entry._corsPolicy; }
  static auto getPreMiddleware(const PathHandlerEntry& entry) { return entry._preMiddleware; }
  static auto getPostMiddleware(const PathHandlerEntry& entry) { return entry._postMiddleware; }
  static auto getNormalMethodBmp(const PathHandlerEntry& entry) { return entry._normalMethodBmp; }
  static auto getAsyncMethodBmp(const PathHandlerEntry& entry) { return entry._asyncMethodBmp; }
  static auto getStreamingMethodBmp(const PathHandlerEntry& entry) { return entry._streamingMethodBmp; }
  static bool hasWebSocketEndpoint(const PathHandlerEntry& entry) { return entry.hasWebSocketEndpoint(); }
  static auto* webSocketEndpointPtr(const PathHandlerEntry& entry) { return entry.webSocketEndpointPtr(); }

  static auto* normalHandlerPtr(const PathHandlerEntry& entry, http::MethodIdx idx) {
    return entry.requestHandlerPtr(idx);
  }
  static auto* asyncHandlerPtr(const PathHandlerEntry& entry, http::MethodIdx idx) {
    return entry.asyncHandlerPtr(idx);
  }
};

TEST_F(PathHandlerEntryTest, SetPathEmpty) {
  EXPECT_THROW(router.setPath(http::Method::GET, "/", RequestHandler{}), std::invalid_argument);
  EXPECT_THROW(router.setPath(http::Method::GET, "/", StreamingHandler{}), std::invalid_argument);
  EXPECT_THROW(router.setPath(http::Method::GET, "/", AsyncRequestHandler{}), std ::invalid_argument);
}

TEST_F(PathHandlerEntryTest, SpecialOperationsWithoutWebSocket) {
  entry = router.setPath(http::Method::GET, "/ctor", MakeNormalHandler());
  addPaths();
  entry.before([](HttpRequest&) { return MiddlewareResult::Continue(); })
      .after([](const HttpRequest&, HttpResponse&) {})
      .cors(CorsPolicy(CorsPolicy::Active::On).allowAnyOrigin());

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
  moved = router.setPath(http::Method::GET, "/ctor", MakeNormalHandler());
  entry = std::move(moved);

  auto& alias = entry;
  entry = alias;  // should do nothing
  entry = std::move(alias);
}

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

TEST_F(PathHandlerEntryTest, CopyAndMoveConstructorsCoverMixedHandlers) {
  auto& entry = router.setPath(http::Method::GET, "/ctor", MakeNormalHandler());
  addPaths();
  entry.before([](HttpRequest&) { return MiddlewareResult::Continue(); })
      .after([](const HttpRequest&, HttpResponse&) {})
      .cors(CorsPolicy(CorsPolicy::Active::On).allowAnyOrigin());

  auto result = router.match(http::Method::GET, "/ctor");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Request);
  EXPECT_NE(result.requestHandler(), nullptr);
  EXPECT_EQ(result.requestMiddlewareRange.size(), 1U);

  result.resetHandler();
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::None);
  EXPECT_FALSE(result.hasHandler());
  result = router.match(http::Method::POST, "/ctor");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Streaming);
  EXPECT_NE(result.streamingHandler(), nullptr);
  EXPECT_EQ(result.responseMiddlewareRange.size(), 1U);
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

TEST_F(PathHandlerEntryTest, CopyAssignmentConstructsAsyncHandler) {
  auto& sourceEntry = router.setPath(http::Method::PUT, "/async-src", MakeAsyncHandler());
  Router target;
  auto& targetEntry = target.setPath(http::Method::DELETE, "/async-target", MakeNormalHandler());

  targetEntry = sourceEntry;
  auto result = target.match(http::Method::PUT, "/async-target");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Async);
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

TEST_F(PathHandlerEntryTest, MoveAssignmentConstructsNewAsyncHandler) {
  auto& sourceEntry = router.setPath(http::Method::PATCH, "/move-async-src", MakeAsyncHandler());
  Router target;
  auto& targetEntry = target.setPath(http::Method::DELETE, "/move-async-target", MakeNormalHandler());

  targetEntry = std::move(sourceEntry);
  auto result = target.match(http::Method::PATCH, "/move-async-target");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Async);
}

TEST_F(PathHandlerEntryTest, CorsAndMiddlewarePopulatedOnMatch) {
  auto& entry = router.setPath(http::Method::GET, "/middleware", MakeNormalHandler());
  entry.cors(CorsPolicy(CorsPolicy::Active::On).allowAnyOrigin())
      .before([](HttpRequest&) { return MiddlewareResult::Continue(); })
      .after([](const HttpRequest&, HttpResponse&) {});

  auto result = router.match(http::Method::GET, "/middleware");
  ASSERT_NE(result.pCorsPolicy, nullptr);
  EXPECT_TRUE(result.pCorsPolicy->active());
  EXPECT_EQ(result.requestMiddlewareRange.size(), 1U);
  EXPECT_EQ(result.responseMiddlewareRange.size(), 1U);
}

TEST_F(PathHandlerEntryTest, NormalAfterStreamingThrows) {
  router.setPath(http::Method::GET, "/conflict", MakeStreamingHandler());
  EXPECT_THROW(router.setPath(http::Method::GET, "/conflict", MakeNormalHandler()), std::logic_error);
}

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

TEST_F(PathHandlerEntryTest, StreamingAfterNormalThrows) {
  router.setPath(http::Method::GET, "/conflict-stream-1", MakeNormalHandler());
  EXPECT_THROW(router.setPath(http::Method::GET, "/conflict-stream-1", MakeStreamingHandler()), std::logic_error);
}

TEST_F(PathHandlerEntryTest, StreamingAfterAsyncThrows) {
  router.setPath(http::Method::GET, "/conflict-stream-2", MakeAsyncHandler());
  EXPECT_THROW(router.setPath(http::Method::GET, "/conflict-stream-2", MakeStreamingHandler()), std::logic_error);
}

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

}  // namespace aeronet