#include "aeronet/path-handler-entry.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <utility>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/request-task.hpp"
#include "aeronet/router.hpp"

using namespace aeronet;

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

TEST(PathHandlerEntryTest, CopyAndMoveConstructorsCoverMixedHandlers) {
  Router router;
  auto& entry = router.setPath(http::Method::GET, "/ctor", MakeNormalHandler());
  router.setPath(http::Method::POST, "/ctor", MakeStreamingHandler());
  router.setPath(http::Method::PUT, "/ctor", MakeAsyncHandler());
  entry.before([](HttpRequest&) { return MiddlewareResult::Continue(); })
      .after([](const HttpRequest&, HttpResponse&) {})
      .cors(CorsPolicy(CorsPolicy::Active::On).allowAnyOrigin());

  PathHandlerEntry copied(entry);
  PathHandlerEntry secondCopy(copied);
  PathHandlerEntry moved(std::move(secondCopy));
  PathHandlerEntry movedAgain(std::move(moved));
  (void)movedAgain;
}

TEST(PathHandlerEntryTest, CopyAssignmentTransfersNormalHandlers) {
  Router source;
  auto& sourceEntry = source.setPath(http::Method::GET, "/copy-src", MakeNormalHandler());
  Router target;
  auto& targetEntry = target.setPath(http::Method::DELETE, "/copy-dst", MakeNormalHandler());

  targetEntry = sourceEntry;
  auto result = target.match(http::Method::GET, "/copy-dst");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Request);
  EXPECT_NE(result.requestHandler(), nullptr);
}

TEST(PathHandlerEntryTest, CopyAssignmentReusesExistingStreamingStorage) {
  Router source;
  auto& sourceEntry = source.setPath(http::Method::POST, "/stream-src", MakeStreamingHandler());
  Router target;
  auto& targetEntry = target.setPath(http::Method::POST, "/stream-target", MakeStreamingHandler());

  targetEntry = sourceEntry;
  auto result = target.match(http::Method::POST, "/stream-target");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Streaming);
}

TEST(PathHandlerEntryTest, CopyAssignmentConstructsNewStreamingHandler) {
  Router source;
  auto& sourceEntry = source.setPath(http::Method::POST, "/stream-src-2", MakeStreamingHandler());
  Router target;
  auto& targetEntry = target.setPath(http::Method::DELETE, "/stream-target-2", MakeStreamingHandler());

  targetEntry = sourceEntry;
  auto result = target.match(http::Method::POST, "/stream-target-2");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Streaming);
}

TEST(PathHandlerEntryTest, CopyAssignmentConstructsAsyncHandler) {
  Router source;
  auto& sourceEntry = source.setPath(http::Method::PUT, "/async-src", MakeAsyncHandler());
  Router target;
  auto& targetEntry = target.setPath(http::Method::DELETE, "/async-target", MakeNormalHandler());

  targetEntry = sourceEntry;
  auto result = target.match(http::Method::PUT, "/async-target");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Async);
}

TEST(PathHandlerEntryTest, MoveAssignmentTransfersStreamingHandlers) {
  Router source;
  auto& sourceEntry = source.setPath(http::Method::POST, "/move-stream-src", MakeStreamingHandler());
  Router target;
  auto& targetEntry = target.setPath(http::Method::POST, "/move-stream-target", MakeStreamingHandler());

  targetEntry = std::move(sourceEntry);
  auto result = target.match(http::Method::POST, "/move-stream-target");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Streaming);
}

TEST(PathHandlerEntryTest, MoveAssignmentConstructsNewAsyncHandler) {
  Router source;
  auto& sourceEntry = source.setPath(http::Method::PATCH, "/move-async-src", MakeAsyncHandler());
  Router target;
  auto& targetEntry = target.setPath(http::Method::DELETE, "/move-async-target", MakeNormalHandler());

  targetEntry = std::move(sourceEntry);
  auto result = target.match(http::Method::PATCH, "/move-async-target");
  EXPECT_EQ(result.handlerKind, Router::RoutingResult::HandlerKind::Async);
}

TEST(PathHandlerEntryTest, CorsAndMiddlewarePopulatedOnMatch) {
  Router router;
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

TEST(PathHandlerEntryTest, NormalAfterStreamingThrows) {
  Router router;
  router.setPath(http::Method::GET, "/conflict", MakeStreamingHandler());
  EXPECT_THROW(router.setPath(http::Method::GET, "/conflict", MakeNormalHandler()), std::logic_error);
}

TEST(PathHandlerEntryTest, NormalAfterAsyncThrows) {
  Router router;
  router.setPath(http::Method::GET, "/conflict-async", MakeAsyncHandler());
  EXPECT_THROW(router.setPath(http::Method::GET, "/conflict-async", MakeNormalHandler()), std::logic_error);
}

TEST(PathHandlerEntryTest, AsyncAfterNormalThrows) {
  Router router;
  router.setPath(http::Method::GET, "/conflict-async-2", MakeNormalHandler());
  EXPECT_THROW(router.setPath(http::Method::GET, "/conflict-async-2", MakeAsyncHandler()), std::logic_error);
}

TEST(PathHandlerEntryTest, AsyncAfterStreamingThrows) {
  Router router;
  router.setPath(http::Method::GET, "/conflict-async-3", MakeStreamingHandler());
  EXPECT_THROW(router.setPath(http::Method::GET, "/conflict-async-3", MakeAsyncHandler()), std::logic_error);
}

TEST(PathHandlerEntryTest, StreamingAfterNormalThrows) {
  Router router;
  router.setPath(http::Method::GET, "/conflict-stream-1", MakeNormalHandler());
  EXPECT_THROW(router.setPath(http::Method::GET, "/conflict-stream-1", MakeStreamingHandler()), std::logic_error);
}

TEST(PathHandlerEntryTest, StreamingAfterAsyncThrows) {
  Router router;
  router.setPath(http::Method::GET, "/conflict-stream-2", MakeAsyncHandler());
  EXPECT_THROW(router.setPath(http::Method::GET, "/conflict-stream-2", MakeStreamingHandler()), std::logic_error);
}