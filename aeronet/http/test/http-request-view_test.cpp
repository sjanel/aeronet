#include "aeronet/http-request-view.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <thread>

#ifdef AERONET_LINUX
#include <sys/resource.h>
#endif

#include "aeronet/async-await-state.hpp"
#include "aeronet/async-resume-poster.hpp"
#endif

#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/connection-info.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/internal/http-response-access.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/unix-dogstatsd-sink.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_GLAZE
#include "aeronet/http-json.hpp"  // IWYU pragma: keep (bodyAs / bodyAsYaml definitions)
#endif

namespace aeronet {

namespace {
// Helper to build a raw HTTP request buffer we can feed into HttpRequestView::setHead
RawChars BuildRaw(std::string_view method, std::string_view target, std::string_view version = "HTTP/1.1",
                  std::string_view extraHeaders = "", bool includeFinalCRLF = true) {
  RawChars str(64 + extraHeaders.size());
  str.append(method);
  str.push_back(' ');
  str.append(target);
  str.push_back(' ');
  str.append(version);
  str.append(http::CRLF);
  str.append("Host: h");
  str.append(http::CRLF);
  str.append(extraHeaders);
  if (includeFinalCRLF) {
    str.append(http::CRLF);
  }
  return str;
}

// Request line (without its CRLF), then a single Host header field with the given value.
RawChars BuildRawWithHost(std::string_view requestLine, std::string_view host) {
  RawChars str(requestLine.size() + host.size() + 16U);
  str.append(requestLine);
  str.append("\r\nHost: ");
  str.append(host);
  str.append(http::DoubleCRLF);
  return str;
}

}  // namespace

class HttpRequestViewTest : public ::testing::Test {
 protected:
  void SetUp() override {
    globalHeaders.append("server: aeronet");
    req._pConnectionInfo = &connectionInfo;
    req._pGlobalHeaders = &globalHeaders;
    req._pCompressionState = &compressionState;

    compressionState.pCompressionConfig = &compressionConfig;
  }

  http::StatusCode reqSet(RawChars raw, bool mergeAllowedForUnknownRequestHeaders = true) {
    inBuffer = std::move(raw);
    req._headParseState = HttpRequestView::HeadParseState::None;  // a new request
    RawChars tmpBuffer;
    auto ret = req.initTrySetHead(inBuffer, tmpBuffer, 4096U, mergeAllowedForUnknownRequestHeaders, nullptr);
    if (ret == http::StatusCodeOK) {
      req.finalizeBeforeHandlerCall({});
    }
    return ret;
  }

  http::StatusCode reqSetWithSpan(RawChars raw, tracing::SpanPtr span,
                                  bool mergeAllowedForUnknownRequestHeaders = true) {
    inBuffer = std::move(raw);
    req._headParseState = HttpRequestView::HeadParseState::None;  // a new request
    RawChars tmpBuffer;
    auto ret = req.initTrySetHead(inBuffer, tmpBuffer, 4096U, mergeAllowedForUnknownRequestHeaders, std::move(span));
    if (ret == http::StatusCodeOK) {
      req.finalizeBeforeHandlerCall({});
    }
    return ret;
  }

  void checkHeaders(std::initializer_list<http::HeaderView> headers) {
    for (const auto& [key, val] : headers) {
      EXPECT_EQ(req.headerValueOrEmpty(LowerAsciiKey{key}), val);
    }
  }

  void rehash(std::size_t capacity) { req._headers.rehash(capacity); }

  void shrinkAndMaybeClear() { req.shrinkAndMaybeClear(); }

  // Appends 'bytes' to the bytes received so far (modified by their parsing) in a new buffer, as when the connection
  // buffer grows. The previous buffer is kept until the next call, overwritten: views not moved to the new buffer read
  // garbage instead of the head.
  void receive(std::string_view bytes) {
    RawChars grown(inBuffer.size() + bytes.size());
    grown.unchecked_append(std::string_view(inBuffer));
    grown.unchecked_append(bytes);
    if (!inBuffer.empty()) {  // data() is null before the first allocation
      std::memset(inBuffer.data(), 'X', inBuffer.size());
    }
    previousInBuffer.swap(inBuffer);
    inBuffer.swap(grown);
  }

  // Receives 'bytes' then attempts to parse the head received so far, as the server does after each read.
  http::StatusCode receiveAndParse(std::string_view bytes) {
    receive(bytes);
    auto ret = req.initTrySetHead(inBuffer, headTmpBuffer, 4096U, true, nullptr);
    if (ret == http::StatusCodeOK) {
      req.finalizeBeforeHandlerCall({});
    }
    return ret;
  }

  void keepHeadForBody() { req.keepHeadForBody(); }

  [[nodiscard]] bool isHeadWaitingForBody() const { return req.isHeadWaitingForBody(); }

  void resumeHeadWaitingForBody() {
    req.resumeHeadWaitingForBody(inBuffer);
    req.finalizeBeforeHandlerCall({});
  }

  // What the parsing of a head produced, to compare the parsing of the same head received in several parts.
  struct HeadSnapshot {
    bool operator==(const HeadSnapshot&) const = default;

    http::Method method{http::Method::GET};
    http::Version version;
    std::string path;
    std::string authority;
    std::vector<std::pair<std::string, std::string>> queryParams;
    std::vector<std::pair<std::string, std::string>> headers;
    std::size_t headSpanSize{};
  };

  [[nodiscard]] HeadSnapshot snapshot() const {
    HeadSnapshot ret{
        req.method(), req.version(), std::string(req.path()), std::string(req.authority()), {}, {}, req.headSpanSize(),
    };
    for (const auto& [key, value] : req.queryParamsRange()) {
      ret.queryParams.emplace_back(key, value);
    }
    for (const auto& [name, value] : req.headers()) {
      ret.headers.emplace_back(name, value);
    }
    std::ranges::sort(ret.headers);
    return ret;
  }

  // Helpers that exercise private internals via friendship with HttpRequestView.
  void setBodyAccessAggregated() { req._bodyAccessMode = HttpRequestView::BodyAccessMode::Aggregated; }
  void setBodyAccessStreamingWithBridgeNoHasMore() {
    req._bodyAccessMode = HttpRequestView::BodyAccessMode::Streaming;
    static HttpRequestView::BodyAccessBridge bridge;
    bridge.readChunk = [](HttpRequestView&, void*, std::size_t) -> std::string_view { return {}; };
    bridge.hasMore = [](const HttpRequestView&, void*) -> bool { return false; };
    req._pBodyAccessBridge = &bridge;
  }

  void setResponsePossibleEncoding(Encoding encoding) { req._responsePossibleEncoding = encoding; }

  void setCompressionState(CompressionState* state) { req._pCompressionState = state; }

  void prefinalizeResponse(HttpResponse& resp, tracing::TelemetryContext& telemetryContext) {
    req.prefinalizeHttpResponse(resp, telemetryContext);
  }

  // Test helpers that require friend access to HttpRequestView private members
  struct FakeSpan : public tracing::Span {
    static inline int64_t lastStatusCode = -1;
    static inline int64_t lastDurationUs = -1;
    static inline bool ended = false;
    static inline bool sawHttpHost = false;
    static inline std::string lastHttpHost;

    void setAttribute(std::string_view key, int64_t val) noexcept override {
      if (key == "http.status_code") {
        lastStatusCode = val;
      } else if (key == "http.duration_us") {
        lastDurationUs = val;
      }
    }

    void setAttribute(std::string_view key, std::string_view val) noexcept override {
      if (key == "http.host") {
        sawHttpHost = true;
        lastHttpHost.assign(val);
      }
    }

    void end() noexcept override { ended = true; }
  };

  static std::string_view bridgeReadChunk(HttpRequestView& /*req*/, void* /*ctx*/, std::size_t /*maxBytes*/) {
    return "chunk-data";
  }

  static bool bridgeHasMore(const HttpRequestView& /*req*/, void* /*ctx*/) { return true; }

  static std::string_view bridgeAggregate(HttpRequestView& /*req*/, void* /*ctx*/) { return {"full-body"}; }

  // Helpers that perform operations requiring friendship (call private members on req)
  void installStreamingBridge() {
    static HttpRequestView::BodyAccessBridge bridge;
    bridge.readChunk = bridgeReadChunk;
    bridge.hasMore = bridgeHasMore;
    req._pBodyAccessBridge = &bridge;
  }

  void installAggregateBridge() {
    static HttpRequestView::BodyAccessBridge bridge;
    bridge.aggregate = bridgeAggregate;
    req._pBodyAccessBridge = &bridge;
  }

#ifdef AERONET_ENABLE_GLAZE
  // Make req.body() return the given bytes via an aggregate bridge (used to feed bodyAs<T>()).
  static inline std::string sAggregatedBody;
  void setAggregatedBody(std::string_view body) {
    sAggregatedBody.assign(body);
    static HttpRequestView::BodyAccessBridge bridge;
    bridge.aggregate = [](HttpRequestView&, void*) -> std::string_view { return sAggregatedBody; };
    req._pBodyAccessBridge = &bridge;
  }
#endif

  // Helper to set a custom BodyAccessBridge and explicitly clear the context.
  void setCustomBridgeWithNullContext(HttpRequestView::BodyAccessBridge::AggregateFn aggregate,
                                      HttpRequestView::BodyAccessBridge::ReadChunkFn readChunk,
                                      HttpRequestView::BodyAccessBridge::HasMoreFn hasMore) {
    static HttpRequestView::BodyAccessBridge sbridge;
    sbridge.aggregate = aggregate;
    sbridge.readChunk = readChunk;
    sbridge.hasMore = hasMore;
    req._pBodyAccessBridge = &sbridge;
    req._pBodyAccessContext = nullptr;
  }

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  void callPinHeadStorage() { req.pinHeadStorage(headBuffer); }
  // Points the request to the state of its async handler, as its protocol does when dispatching it.
  void setAsyncAwait(AsyncAwaitState* pAsyncAwait) { req._pAsyncAwait = pAsyncAwait; }
  [[nodiscard]] AsyncResumePoster callAsyncResumePoster() const { return req.asyncResumePoster(); }
#endif

  // Helper to set a header view pointing at arbitrary bytes (fixture has friend access)
  void setHeaderViewToPtr(std::string_view key, const char* dataPtr, std::size_t len) {
    req._headers.try_emplace(key, dataPtr, len);
  }
  void setAddTrailerHeader(bool addTrailerHeader) { req._addTrailerHeader = addTrailerHeader; }

  void setTrailerViewToPtr(std::string_view key, const char* dataPtr, std::size_t len) {
    req._trailers.try_emplace(key, dataPtr, len);
  }

  void setPathParamToPtr(std::string_view key, const char* dataPtr, std::size_t len) {
    req._pathParams.try_emplace(key, dataPtr, len);
  }

  [[nodiscard]] bool callClientKeepsAliveForHttp1() const { return req.clientKeepsAliveForHttp1(); }

  void attachSpan(tracing::SpanPtr span) { req._traceSpan = std::move(span); }

  void callEnd(http::StatusCode sc) { req.end(sc); }

  // Feed raw bytes to HttpRequestView parser and ensure no crash/UB
  void fuzzHttpRequestParsing(const RawChars& input, bool mergeUnknownHeaders = true) {
    if (input.size() < http::kHttpReqLineMinLen) {
      return;  // the server does not parse a head before the minimal request line is received
    }
    inBuffer = input;  // reset
    [[maybe_unused]] auto status = reqSet(input, mergeUnknownHeaders);
  }

  ConcatenatedHeaders globalHeaders;
  CompressionConfig compressionConfig;
  CompressionState compressionState;
  HttpRequestView req;
  ConnectionInfo connectionInfo;
  // Input of the connection, where the request is parsed
  RawChars inBuffer;
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Storage of the head pinned for an async handler
  RawChars headBuffer;
#endif
  RawChars previousInBuffer;
  RawChars headTmpBuffer;
};

TEST_F(HttpRequestViewTest, ReadBodyWithZeroMaxBytesReturnsEmpty) {
  setBodyAccessStreamingWithBridgeNoHasMore();
  auto chunk = req.readBody(0);
  EXPECT_TRUE(chunk.empty());
}

#ifdef AERONET_POSIX
TEST_F(HttpRequestViewTest, PrefinalizeCompressionExceedsMaxRatioIncrementsMetric) {
  compressionConfig.minBytes = 1U;
  compressionConfig.maxCompressRatio = 0.01F;
  compressionState = CompressionState(compressionConfig);
  setCompressionState(&compressionState);

  for (Encoding encoding : test::SupportedEncodings()) {
    test::UnixDogstatsdSink sink;
    TelemetryConfig tcfg;
    tcfg.withDogStatsdSocketPath(sink.path()).withDogStatsdNamespace("svc").enableDogStatsDMetrics(true);
    tracing::TelemetryContext telemetryContext(tcfg);

    setResponsePossibleEncoding(encoding);

    HttpResponse resp(http::StatusCodeOK);
    auto body = test::MakeRandomPayload(2UL << 10U);
    resp.body(body, http::ContentTypeTextPlain);

    prefinalizeResponse(resp, telemetryContext);
    telemetryContext.flush();

    EXPECT_EQ(sink.recvMessage(), "svc.aeronet.http_responses.compression.exceeds_max_ratio_total:1|c");
  }
}
#endif

// 1xx, 204 and 304 responses end at the empty line after their headers (RFC 9112 §6.3): the body set by a handler, its
// headers and its trailers are dropped before the response is serialized, for HTTP/1.1 and HTTP/2 alike.
TEST_F(HttpRequestViewTest, PrefinalizeDropsBodyOfStatusesWithoutContent) {
  static constexpr std::string_view kBigBody =
      "a body long enough not to be inlined, whatever the minimum size of the captured bodies";
  tracing::TelemetryContext telemetryContext{TelemetryConfig{}};

  for (std::string_view method : {"GET", "HEAD"}) {
    ASSERT_EQ(reqSet(BuildRaw(method, "/test", "HTTP/1.1")), http::StatusCodeOK);
    for (http::StatusCode status : {http::StatusCodeNoContent, http::StatusCodeNotModified, http::StatusCode{199}}) {
      for (bool captured : {false, true}) {
        SCOPED_TRACE(std::format("{} {} captured={}", method, status, captured));
        HttpResponse resp(status);
        if (captured) {
          resp.body(std::string(kBigBody));
        } else {
          resp.body("body");
        }
        resp.trailerAddLine("x-trailer", "t").headerAddLine("x-custom", "kept");

        prefinalizeResponse(resp, telemetryContext);

        EXPECT_EQ(resp.status(), status);
        EXPECT_FALSE(resp.hasBody());
        EXPECT_TRUE(resp.bodyInMemory().empty());
        EXPECT_EQ(resp.bodyLength(), 0U);
        EXPECT_EQ(resp.trailersSize(), 0U);
        EXPECT_FALSE(resp.headerValue(http::ContentType).has_value());
        EXPECT_FALSE(resp.headerValue(http::ContentLength).has_value());
        EXPECT_EQ(resp.headerValueOrEmpty("x-custom"), "kept");

        // The response stays usable: a body set afterwards is a regular one.
        resp.status(http::StatusCodeOK).body("again");
        EXPECT_EQ(resp.bodyInMemory(), "again");
      }
    }
  }
}

TEST_F(HttpRequestViewTest, PrefinalizeKeepsBodyOfStatusesWithContent) {
  tracing::TelemetryContext telemetryContext{TelemetryConfig{}};
  ASSERT_EQ(reqSet(BuildRaw("GET", "/test", "HTTP/1.1")), http::StatusCodeOK);

  for (http::StatusCode status : {
           http::StatusCodeOK,
           http::StatusCodeResetContent,
           http::StatusCodeNotFound,
           http::StatusCodeInternalServerError,
       }) {
    HttpResponse resp(status, "body");
    prefinalizeResponse(resp, telemetryContext);
    EXPECT_EQ(resp.bodyInMemory(), "body") << status;
    EXPECT_TRUE(resp.headerValue(http::ContentType).has_value()) << status;
  }
}

TEST_F(HttpRequestViewTest, BridgeWithNullContextAggregateHandledGracefully) {
  // Aggregate accessor: null context -> empty body
  using AggFnRaw = std::string_view (*)(HttpRequestView&, void*);
  AggFnRaw agg = +[](HttpRequestView& /*r*/, void* ctx) -> std::string_view {
    if (ctx == nullptr) {
      return {};
    }
    return "full";
  };

  // For aggregate test we don't need readChunk/hasMore
  setCustomBridgeWithNullContext(agg, /*readChunk=*/nullptr, /*hasMore=*/nullptr);

  // body() with null context should not crash and should return empty
  EXPECT_TRUE(req.body().empty());
  // hasMoreBody should be false when aggregated
  EXPECT_FALSE(req.hasMoreBody());
}

TEST_F(HttpRequestViewTest, BridgeWithNullContextStreamingHandledGracefully) {
  // Streaming accessor: null context -> empty chunks and hasMore false
  using ReadFnRaw = std::string_view (*)(HttpRequestView&, void*, std::size_t);
  using HasMoreFnRaw = bool (*)(const HttpRequestView&, void*);

  ReadFnRaw rd = +[](HttpRequestView& /*r*/, void* ctx, std::size_t /*maxBytes*/) -> std::string_view {
    if (ctx == nullptr) {
      return {};
    }
    return "c";
  };
  HasMoreFnRaw hm = +[](const HttpRequestView& /*r*/, void* ctx) -> bool { return ctx != nullptr; };

  setCustomBridgeWithNullContext(nullptr, rd, hm);

  // readBody should return empty for null context (streaming)
  EXPECT_TRUE(req.readBody(4).empty());
  EXPECT_FALSE(req.hasMoreBody());
}

TEST_F(HttpRequestViewTest, BridgePresentButAggregateNull) {
  // Removed: test constructed a BodyAccessBridge directly which uses private
  // types; use BridgePointerPresentButAggregateNull which installs the bridge
  // via the provided helper instead.
}

TEST_F(HttpRequestViewTest, BridgePointerPresentButAggregateNull) {
  // Install a bridge where the bridge pointer is non-null but aggregate is
  // explicitly null. Use the helper that sets the bridge and nulls the
  // context. This should exercise the `_bodyAccessBridge != nullptr` true
  // path while `aggregate == nullptr` is false, so body() must not call
  // aggregate and should return an empty view.
  setCustomBridgeWithNullContext(/*aggregate=*/nullptr, /*readChunk=*/nullptr, /*hasMore=*/nullptr);

  EXPECT_TRUE(req.body().empty());
  EXPECT_FALSE(req.hasMoreBody());
}

TEST_F(HttpRequestViewTest, TraceSpanNotSetWhenNoHostHeader) {
  // Build a raw request without a Host header and pass a FakeSpan into
  // initTrySetHead; the span should receive http.method, http.target, and
  // http.scheme but not http.host.
  RawChars raw;
  raw.append("GET /nohost HTTP/1.0\r\n");
  raw.append(MakeHttp1HeaderLine(http::Connection, "close"));
  raw.append(http::CRLF);

  // Reset FakeSpan marker
  FakeSpan::sawHttpHost = false;

  auto span = std::make_unique<FakeSpan>();
  auto status = reqSetWithSpan(std::move(raw), std::move(span));
  ASSERT_EQ(status, http::StatusCodeOK);

  // Since no Host header was present, the span should not have seen http.host
  EXPECT_FALSE(FakeSpan::sawHttpHost);
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS

TEST_F(HttpRequestViewTest, PinHead_NoHeadSpan_Noop) {
  // When no head span is present, pinHeadStorage should be a no-op.
  EXPECT_EQ(req.headSpanSize(), 0);

  callPinHeadStorage();

  EXPECT_EQ(req.headSpanSize(), 0);
}

TEST_F(HttpRequestViewTest, PinHead_NormalCopiesAndRemaps) {
  // Build a simple request and initialize head so _headSpanSize is set
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1", "X-Test: v\r\n");
  auto st = reqSet(std::move(raw));
  ASSERT_EQ(st, http::StatusCodeOK);

  // Before pinning, header views point into inBuffer
  auto before = req.headerValueOrEmpty("x-test");
  EXPECT_EQ(before, "v");

  // Call pinHeadStorage to copy head into headBuffer and remap views
  callPinHeadStorage();
  EXPECT_GT(req.headSpanSize(), 0);

  // Header value should still be accessible and unchanged after pin
  EXPECT_EQ(req.headerValueOrEmpty("x-test"), "v");
}

TEST_F(HttpRequestViewTest, PinHead_SecondCallIsNoop) {
  // Build and pin once
  auto raw = BuildRaw("GET", "/p2", "HTTP/1.1", "X-A: b\r\n");
  auto st = reqSet(std::move(raw));
  ASSERT_EQ(st, http::StatusCodeOK);
  callPinHeadStorage();
  EXPECT_GT(req.headSpanSize(), 0);

  // Capture header pointer after first pin
  auto val1 = req.headerValueOrEmpty("x-a");

  // Second call should be a no-op and not crash/change values
  callPinHeadStorage();
  EXPECT_GT(req.headSpanSize(), 0);
  auto val2 = req.headerValueOrEmpty("x-a");
  EXPECT_EQ(val1, val2);
}

TEST_F(HttpRequestViewTest, PinHead_AbsoluteFormKeepsItsHost) {
  ASSERT_EQ(reqSet(BuildRaw("GET", "http://example.com:81/p?x=1", "HTTP/1.1", "X-Test: v\r\n")), http::StatusCodeOK);
  const HeadSnapshot expected = snapshot();

  callPinHeadStorage();
  // The connection buffer is reused for other requests while the async handler is suspended.
  std::ranges::fill(inBuffer, 'X');

  EXPECT_EQ(snapshot(), expected);
  EXPECT_EQ(req.path(), "/p");
  EXPECT_EQ(req.authority(), "example.com:81");
  EXPECT_EQ(req.headerValueOrEmpty("host"), "example.com:81");
  EXPECT_EQ(req.queryParamValueOrEmpty("x"), "1");
  EXPECT_EQ(req.headerValueOrEmpty("x-test"), "v");
}

TEST_F(HttpRequestViewTest, HasMoreBodyWhileAsyncHandlerWaitsForBody) {
  // Without body bridge nor async handler, there is no more body.
  EXPECT_FALSE(req.hasMoreBody());

  AsyncAwaitState asyncAwait;
  setAsyncAwait(&asyncAwait);
  EXPECT_FALSE(req.hasMoreBody());

  // The async handler started before the body was received.
  asyncAwait.needsBody = true;
  EXPECT_TRUE(req.hasMoreBody());

  // The next request is not served by the async handler anymore.
  ASSERT_EQ(reqSet(BuildRaw("GET", "/next")), http::StatusCodeOK);
  EXPECT_FALSE(req.hasMoreBody());
}

namespace {

// Resume poster of the deferred work tests: raises the std::atomic<bool> of its context.
void RaisePostedFlag(const AsyncResumePoster& poster, std::coroutine_handle<> /*handle*/) {
  static_cast<std::atomic<bool>*>(poster.pContext)->store(true, std::memory_order_release);
}

// Resume poster simulating a broken event-loop channel: raises the std::atomic<bool> of its context, then throws.
void RaisePostedFlagThenThrow(const AsyncResumePoster& poster, std::coroutine_handle<> handle) {
  RaisePostedFlag(poster, handle);
  throw std::runtime_error("connection closed while background work was running");
}

}  // namespace

TEST_F(HttpRequestViewTest, AwaitablesUpdateTheAsyncAwaitState) {
  AsyncAwaitState asyncAwait;
  std::atomic<bool> posted{false};
  asyncAwait.resumePoster = {.pPost = &RaisePostedFlag, .pContext = &posted};
  setAsyncAwait(&asyncAwait);

  // The body is not received yet: the body awaitables suspend until it is.
  asyncAwait.needsBody = true;
  auto bodyAwaitable = req.bodyAwaitable();
  EXPECT_FALSE(bodyAwaitable.await_ready());
  bodyAwaitable.await_suspend(std::coroutine_handle<>{});
  EXPECT_EQ(asyncAwait.reason, AsyncAwaitState::Reason::WaitingForBody);

  auto chunkAwaitable = req.readBodyAsync(16);
  EXPECT_FALSE(chunkAwaitable.await_ready());
  asyncAwait.reason = AsyncAwaitState::Reason::None;
  chunkAwaitable.await_suspend(std::coroutine_handle<>{});
  EXPECT_EQ(asyncAwait.reason, AsyncAwaitState::Reason::WaitingForBody);

  // The poster of the request is the one of its async handler.
  const AsyncResumePoster poster = callAsyncResumePoster();
  poster(std::coroutine_handle<>{});
  EXPECT_TRUE(posted.load(std::memory_order_acquire));
}

// When the resume poster throws (e.g. connection closed while background work was running),
// DeferredWorkAwaitable::await_suspend swallows the exception and calls log::error.
// Verified here by confirming no exception escapes and the code reaches the error path.
TEST_F(HttpRequestViewTest, DeferredWork_PostCallbackThrows_AbsorbedAndLogged) {
  // The poster will throw, simulating a broken event-loop channel.
  std::atomic<bool> postCallbackInvoked{false};
  AsyncAwaitState asyncAwait;
  asyncAwait.resumePoster = {.pPost = &RaisePostedFlagThenThrow, .pContext = &postCallbackInvoked};
  setAsyncAwait(&asyncAwait);

  // Manually drive await_suspend; it spawns a detached background thread.
  auto awaitable = req.deferWork([] { return 42; });
  awaitable.await_suspend(std::coroutine_handle<>{});

  // Wait for the background thread to reach postCallback (generous 5-second timeout).
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!postCallbackInvoked.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(postCallbackInvoked.load()) << "Background thread should have called postCallback";

  // Allow the catch block (log::error call) to finish before locals are destroyed.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // If we reach here without a crash, the throwing path is handled correctly.
  EXPECT_EQ(asyncAwait.reason, AsyncAwaitState::Reason::WaitingForCallback);
}

TEST_F(HttpRequestViewTest, DeferredWorkCompletionOutlivesAwaitableStorage) {
  std::atomic<bool> workStarted{false};
  std::atomic<bool> finishWork{false};
  std::atomic<bool> postCallbackInvoked{false};
  AsyncAwaitState asyncAwait;
  asyncAwait.resumePoster = {.pPost = &RaisePostedFlag, .pContext = &postCallbackInvoked};
  setAsyncAwait(&asyncAwait);

  {
    auto awaitable = req.deferWork([&] {
      workStarted.store(true, std::memory_order_release);
      while (!finishWork.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      return std::string(1024, 'x');
    });
    awaitable.await_suspend(std::coroutine_handle<>{});

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!workStarted.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    ASSERT_TRUE(workStarted.load(std::memory_order_acquire));
  }

  finishWork.store(true, std::memory_order_release);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!postCallbackInvoked.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_TRUE(postCallbackInvoked.load(std::memory_order_acquire));
  EXPECT_EQ(asyncAwait.reason, AsyncAwaitState::Reason::WaitingForCallback);
}

#ifdef AERONET_LINUX
TEST_F(HttpRequestViewTest, DeferredWorkThreadCreationFailureIsRethrownByAwaitResume) {
  AsyncAwaitState asyncAwait;
  asyncAwait.resumePoster = {.pPost = [](const AsyncResumePoster&, std::coroutine_handle<>) {}};
  setAsyncAwait(&asyncAwait);

  // Run in a forked child: once its user is not allowed any new thread, none can be started (a privileged user ignores
  // the limit, the test is then skipped).
  const auto deferWorkWithoutThreads = [this, &asyncAwait] {
    const rlimit noNewThread{0, 0};
    if (::setrlimit(RLIMIT_NPROC, &noNewThread) != 0) {
      std::fputs("skipped\n", stderr);
      std::_Exit(0);
    }
    auto awaitable = req.deferWork([] { return 42; });
    if (awaitable.await_suspend(std::coroutine_handle<>{})) {
      std::fputs("skipped\n", stderr);
      std::_Exit(0);
    }
    // Not suspended: the coroutine continues right away, without waiting for a completion.
    if (asyncAwait.reason != AsyncAwaitState::Reason::None) {
      std::_Exit(2);
    }
    try {
      (void)awaitable.await_resume();
    } catch (const std::system_error&) {
      std::fputs("rethrown\n", stderr);
      std::_Exit(0);
    }
    std::_Exit(1);
  };
  EXPECT_EXIT(deferWorkWithoutThreads(), ::testing::ExitedWithCode(0), "rethrown|skipped");
}
#endif

#endif

TEST_F(HttpRequestViewTest, NoCRLF) { EXPECT_EQ(reqSet(RawChars("GET /some-path HTTP/1.1")), 0); }

TEST_F(HttpRequestViewTest, TrailingRequestLineCRNeedsMoreData) {
  EXPECT_EQ(reqSet(RawChars("GET /some-path HTTP/1.1\r")), 0);
}

TEST_F(HttpRequestViewTest, BareRequestLineCRIsRejectedImmediately) {
  EXPECT_EQ(reqSet(RawChars("GET / HTTP/1.1\rX")), http::StatusCodeBadRequest);
}

TEST_F(HttpRequestViewTest, NotEnoughDataOnlyFirstLine) { EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\n")), 0); }

TEST_F(HttpRequestViewTest, NotEnoughDataNoEndOfHeaders) {
  EXPECT_EQ(reqSet(BuildRaw("GET", "/", "HTTP/1.1", "Server: aeronet", false)), 0);
}

TEST_F(HttpRequestViewTest, TrailingHeaderCRNeedsMoreData) {
  EXPECT_EQ(reqSet(RawChars("GET / HTTP/1.0\r\nKey: value\r")), 0);
}

TEST_F(HttpRequestViewTest, BareHeaderCRIsRejectedImmediately) {
  EXPECT_EQ(reqSet(RawChars("GET / HTTP/1.0\r\nKey: value\rX")), http::StatusCodeBadRequest);
}

TEST_F(HttpRequestViewTest, InvalidHttpVersion) {
  EXPECT_EQ(reqSet(BuildRaw("GET", "/", "HTTP")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /some-path HTTP1.1\r\n\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /some-path\r\n\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /some-path \r\n\r\n")), http::StatusCodeBadRequest);
}

TEST_F(HttpRequestViewTest, InvalidMethod) {
  EXPECT_EQ(reqSet(RawChars("GETA / HTTP/1.1\r\n\r\n")), http::StatusCodeNotImplemented);
}

// RFC 9110 §9.1: the method token is case-sensitive.
TEST_F(HttpRequestViewTest, MethodIsCaseSensitive) {
  for (std::string_view method : {"get", "Get", "gET", "post", "Head", "delete", "OPTIONs", "connect", "pAtCh"}) {
    EXPECT_EQ(reqSet(BuildRaw(method, "/")), http::StatusCodeNotImplemented) << method;
  }
  for (std::string_view method : {"GET", "POST", "PUT", "DELETE", "HEAD", "OPTIONS", "PATCH", "TRACE"}) {
    ASSERT_EQ(reqSet(BuildRaw(method, "/")), http::StatusCodeOK) << method;
    EXPECT_EQ(http::MethodToStr(req.method()), method);
  }
}

TEST_F(HttpRequestViewTest, EmptyMethodIsRejected) {
  EXPECT_EQ(reqSet(BuildRaw("", "/")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("  GET / HTTP/1.1\r\nHost: h\r\n\r\n")), http::StatusCodeBadRequest);
}

TEST_F(HttpRequestViewTest, InvalidPath) {
  EXPECT_EQ(reqSet(RawChars("GET   HTTP/1.1\r\n\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET ?a=b HTTP/1.1\r\n\r\n")), http::StatusCodeBadRequest);
}

// RFC 9112 §3.2: request-target = origin-form / absolute-form / authority-form / asterisk-form.
TEST_F(HttpRequestViewTest, RequestTargetForms) {
  EXPECT_EQ(reqSet(BuildRaw("GET", "!")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(BuildRaw("GET", "a")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(BuildRaw("GET", "1http://a/")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(BuildRaw("GET", "ht_tp://a/")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(BuildRaw("GET", "http/a:b")), http::StatusCodeBadRequest);  // ':' is not the end of the scheme
  EXPECT_EQ(reqSet(BuildRaw("GET", "*")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(BuildRaw("OPTIONS", "*/")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(BuildRaw("CONNECT", "?a")), http::StatusCodeBadRequest);  // empty path

  EXPECT_EQ(reqSet(BuildRaw("OPTIONS", "*")), http::StatusCodeOK);
  EXPECT_EQ(req.path(), "*");
  EXPECT_EQ(reqSet(BuildRaw("CONNECT", "example.com:443")), http::StatusCodeOK);
  EXPECT_EQ(req.path(), "example.com:443");
  EXPECT_EQ(reqSet(BuildRaw("GET", "http://a/b")), http::StatusCodeOK);
  EXPECT_EQ(reqSet(BuildRaw("GET", "coap+tcp.x-y://a/b")), http::StatusCodeOK);
  EXPECT_EQ(reqSet(BuildRaw("GET", "h2c://a/b")), http::StatusCodeOK);
  EXPECT_EQ(reqSet(BuildRaw("GET", "Z09azAZ://a/b")), http::StatusCodeOK);
}

// RFC 9112 §3.2.2: a server accepts the absolute-form, and takes the host from the request-target (§3.3), ignoring
// Host.
TEST_F(HttpRequestViewTest, AbsoluteFormTakesTheHostFromTheTarget) {
  struct Case {
    std::string_view target;
    std::string_view authority;
    std::string_view path;
    std::string_view queryX;
  };
  static constexpr Case kCases[] = {
      {"http://example.com:8080/onlyget?x=1&y=2", "example.com:8080", "/onlyget", "1"},
      {"http://h/onlyget", "h", "/onlyget", ""},
      {"http://h", "h", "/", ""},  // an empty path stands for "/" (RFC 9110 §4.2.3)
      {"http://h?x=1", "h", "/", "1"},
      {"http://h/", "h", "/", ""},
      {"http://h:80/a%2Eb/%41?x=a%20b+c", "h:80", "/a.b/A", "a b c"},
      {"HTTP://Mixed.Example/Path", "Mixed.Example", "/Path", ""},  // the host is kept as is
      {"http://user:pw@host:8080/p?x=1", "host:8080", "/p", "1"},   // the userinfo is not part of the host
      {"http://@h/p", "h", "/p", ""},
      {"http://[::1]:8080/p", "[::1]:8080", "/p", ""},
      {"http://h/p?x=http://other/z", "h", "/p", "http://other/z"},
      {"coap+tcp.x-y://a/b", "a", "/b", ""},
      {"h://a/a/very/long/path/with/many/segments?x=1&y=2&z=3", "a", "/a/very/long/path/with/many/segments", "1"},
      {"http://a-very-long-host-name.example.com:12345/p", "a-very-long-host-name.example.com:12345", "/p", ""},
      {"http://h:/p", "h:", "/p", ""},                // an empty port is allowed (RFC 3986 §3.2.3)
      {"http://my_h%41st/p", "my_h%41st", "/p", ""},  // the host is not decoded
      {"http://u%40:p!w@[2001:db8::7]:1/p", "[2001:db8::7]:1", "/p", ""},
      {"http://[v1.x]/p", "[v1.x]", "/p", ""},
  };
  for (const Case& testCase : kCases) {
    SCOPED_TRACE(testCase.target);
    ASSERT_EQ(reqSet(BuildRaw("GET", testCase.target)), http::StatusCodeOK);  // with a "Host: h" header
    EXPECT_EQ(req.path(), testCase.path);
    EXPECT_EQ(req.authority(), testCase.authority);
    EXPECT_EQ(req.headerValueOrEmpty("host"), testCase.authority);
    EXPECT_EQ(req.queryParamValueOrEmpty("x"), testCase.queryX);
    EXPECT_EQ(req.method(), http::Method::GET);
    EXPECT_EQ(req.version(), http::HTTP_1_1);
  }

  // The next request of the connection, in origin-form, takes its host from Host.
  ASSERT_EQ(reqSet(BuildRaw("GET", "/plain?x=2")), http::StatusCodeOK);
  EXPECT_EQ(req.path(), "/plain");
  EXPECT_TRUE(req.authority().empty());
  EXPECT_EQ(req.headerValueOrEmpty("host"), "h");
  EXPECT_EQ(req.queryParamValueOrEmpty("x"), "2");
}

TEST_F(HttpRequestViewTest, AbsoluteFormMalformedTargetIsRejected) {
  for (const std::string_view target : {
           "http:",
           "http:/p",
           "http:p",
           "mailto:a@b",
           "http://",
           "http://?x=1",
           "http:///p",
           "http://@/p",
           "http://u@/p",
           "http://a@b@c/p",
           "http://h#f",
           "http://h#f/p",
           "http://\x01h/p",
           "http://u\x7F@h/p",
           "http://h\x1F/p",
           "http://:80/p",
           "http://u@:80/p",
           "http://h:8x/p",
           "http://h:80:90/p",
           "http://h\\x/p",
           "http://h%zz/p",
           "http://h\"/p",
           "http://h<x>/p",
           "http://h|x/p",
           "http://h{x}/p",
           "http://h^/p",
           "http://caf\xC3\xA9/p",
           "http://[::1/p",
           "http://[::1]x/p",
           "http://[::1]:8x/p",
           "http://[1.2.3.4]/p",
           "http://[]/p",
       }) {
    SCOPED_TRACE(target);
    EXPECT_EQ(reqSet(BuildRaw("GET", target)), http::StatusCodeBadRequest);
  }
  // CONNECT keeps its authority-form, whatever the target looks like.
  EXPECT_EQ(reqSet(BuildRaw("CONNECT", "http://h/p")), http::StatusCodeOK);
  EXPECT_EQ(req.path(), "http://h/p");
  EXPECT_TRUE(req.authority().empty());
}

// The Host header field is required for HTTP/1.1 and not repeated, whatever the target, but the host comes from the
// absolute-form target (RFC 9112 §3.2.2, §3.3).
TEST_F(HttpRequestViewTest, AbsoluteFormHostHeaderField) {
  EXPECT_EQ(reqSet(RawChars("GET http://a/p HTTP/1.1\r\nX-Header: value\r\n\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET http://a/p HTTP/1.1\r\nHost: b\r\nHost: c\r\n\r\n")), http::StatusCodeBadRequest);

  ASSERT_EQ(reqSet(RawChars("GET http://a:1/p HTTP/1.1\r\nHost:\r\n\r\n")), http::StatusCodeOK);  // empty Host
  EXPECT_EQ(req.headerValueOrEmpty("host"), "a:1");

  ASSERT_EQ(reqSet(RawChars("GET http://a:1/p HTTP/1.0\r\nX-Header: value\r\n\r\n")), http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("host"), "a:1");
  EXPECT_EQ(req.authority(), "a:1");
  EXPECT_EQ(req.headerValueOrEmpty("x-header"), "value");
  EXPECT_EQ(req.headers().size(), 2U);
}

// RFC 9112 §3.2: a request whose Host header field is not uri-host [ ":" port ] (RFC 9110 §7.2) is refused, whatever
// its version and the form of its request-target. The redirect to https and the handlers build URLs from it.
TEST_F(HttpRequestViewTest, InvalidHostHeaderFieldIsRejected) {
  for (std::string_view host : {
           "a b/c@d",
           "evil.example/x",
           "evil.example?x",
           "evil.example#x",
           "user@h",
           "h:8x",
           ":80",
           "h:80:90",
           "[::1",
           "[::1]x",
           "[]",
           "[1.2.3.4]",
           "a\\b",
           "a\tb",
           "h%zz",
           "caf\xC3\xA9",
           "h\x7F",
       }) {
    SCOPED_TRACE(host);
    for (std::string_view requestLine : {
             "GET / HTTP/1.1",
             "GET / HTTP/1.0",
             "GET http://a/p HTTP/1.1",
             "CONNECT a:443 HTTP/1.1",
             "OPTIONS * HTTP/1.1",
         }) {
      EXPECT_EQ(reqSet(BuildRawWithHost(requestLine, host)), http::StatusCodeBadRequest) << requestLine;
    }
  }
}

TEST_F(HttpRequestViewTest, ValidHostHeaderFieldIsKeptAsIs) {
  for (std::string_view host : {
           "",
           "h",
           "h:80",
           "h:",
           "Example.COM:8080",
           "my_host-1",
           "%41",
           "1.2.3.4:80",
           "[::1]",
           "[2001:db8::7]:8443",
           "[::ffff:192.0.2.1]",
           "[v1.x]",
           "a!$&'()*+,;=~",
       }) {
    SCOPED_TRACE(host);
    ASSERT_EQ(reqSet(BuildRawWithHost("GET /p HTTP/1.1", host)), http::StatusCodeOK);
    ASSERT_TRUE(req.headerValue("host"));
    EXPECT_EQ(*req.headerValue("host"), host);
    EXPECT_EQ(req.path(), "/p");
  }
  // Optional whitespace around the value is not part of it.
  ASSERT_EQ(reqSet(RawChars("GET / HTTP/1.1\r\nHost: \t h:1 \t\r\n\r\n")), http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("host"), "h:1");
}

TEST_F(HttpRequestViewTest, AbsoluteFormHostIsInTheTraceSpan) {
  ASSERT_EQ(reqSetWithSpan(BuildRaw("GET", "http://example.com:81/p"), std::make_unique<FakeSpan>()),
            http::StatusCodeOK);
  EXPECT_EQ(FakeSpan::lastHttpHost, "example.com:81");
}

// Control characters are never valid in a request-target, whatever its form.
TEST_F(HttpRequestViewTest, ControlCharactersInRequestTargetAreRejected) {
  for (int ch = 0; ch < 0x20; ++ch) {
    if (ch == '\r') {
      continue;  // tested as a request line terminator
    }
    RawChars inPath("GET /a");
    inPath.push_back(static_cast<char>(ch));
    inPath.append("b?c=d HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_EQ(reqSet(std::move(inPath)), http::StatusCodeBadRequest) << "byte " << ch;

    RawChars inQuery("GET /a?c=d");
    inQuery.push_back(static_cast<char>(ch));
    inQuery.append(" HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_EQ(reqSet(std::move(inQuery)), http::StatusCodeBadRequest) << "byte " << ch;
  }
  EXPECT_EQ(reqSet(BuildRaw("GET", "/a\x7F")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(BuildRaw("CONNECT", "a\x01:443")), http::StatusCodeBadRequest);
  // bytes above 0x7F are tolerated (raw UTF-8 paths)
  EXPECT_EQ(reqSet(BuildRaw("GET", "/caf\xC3\xA9")), http::StatusCodeOK);
}

// A decoded NUL would truncate the path for C APIs (file system...) while middleware would check the full path.
TEST_F(HttpRequestViewTest, DecodedNulInPathIsRejected) {
  EXPECT_EQ(reqSet(BuildRaw("GET", "/.env%00.txt")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(BuildRaw("GET", "/%00")), http::StatusCodeBadRequest);
  // other decoded control characters stay the business of the application
  EXPECT_EQ(reqSet(BuildRaw("GET", "/a%0Ab")), http::StatusCodeOK);
  EXPECT_EQ(req.path(), "/a\nb");
}

// RFC 3986 §6.2.2.2: an encoded slash is not equivalent to a path separator, so it must not be routed as one.
TEST_F(HttpRequestViewTest, EncodedSlashInPathIsRejected) {
  for (std::string_view target : {"/a%2Fb", "/a%2fb", "/%2F", "/a/b%2F", "/a%2F%2Fb", "http://h/a%2Fb"}) {
    EXPECT_EQ(reqSet(BuildRaw("GET", target)), http::StatusCodeBadRequest) << target;
  }
  // only the path is concerned, and other reserved characters stay decoded
  EXPECT_EQ(reqSet(BuildRaw("GET", "/a/b?x=%2F")), http::StatusCodeOK);
  EXPECT_EQ(req.path(), "/a/b");
  EXPECT_EQ(req.queryParamValueOrEmpty("x"), "/");
  EXPECT_EQ(reqSet(BuildRaw("GET", "/a%3Fb%23c")), http::StatusCodeOK);
  EXPECT_EQ(req.path(), "/a?b#c");
}

namespace {

constexpr auto kNeedMoreData = static_cast<http::StatusCode>(0);  // HttpRequestView::kStatusNeedMoreData

// Parsing modifies the head in place: decoded request-target ('&' and '=' of the query become separators, "%25" a '%'),
// lower-cased header names and Content-Encoding value, merged duplicate header values.
constexpr std::string_view kHeadModifiedByItsParsing =
    "GET /p%2561th/x?a=1&b=%20x+y&flag HTTP/1.1\r\n"
    "Host: h\r\n"
    "X-Dup: v1\r\n"
    "Accept: text/html\r\n"
    "X-Dup: v2\r\n"
    "Content-Encoding: GZIP\r\n"
    "\r\n";

}  // namespace

TEST_F(HttpRequestViewTest, HeadReceivedInTwoPartsIsParsedOnce) {
  ASSERT_EQ(reqSet(RawChars(kHeadModifiedByItsParsing)), http::StatusCodeOK);
  const HeadSnapshot expected = snapshot();
  EXPECT_EQ(expected.path, "/p%61th/x");  // decoded once
  EXPECT_EQ(req.queryParamValueOrEmpty("b"), " x y");
  EXPECT_EQ(req.headerValueOrEmpty("x-dup"), "v1,v2");
  EXPECT_EQ(req.headerValueOrEmpty("content-encoding"), "gzip");

  static_assert(kHeadModifiedByItsParsing.size() > http::kHttpReqLineMinLen);
  // The server parses a head only once the minimal request line is received.
  for (std::size_t cut = http::kHttpReqLineMinLen; cut < kHeadModifiedByItsParsing.size(); ++cut) {
    SCOPED_TRACE(cut);
    inBuffer.clear();
    ASSERT_EQ(receiveAndParse(kHeadModifiedByItsParsing.substr(0, cut)), kNeedMoreData);
    ASSERT_EQ(receiveAndParse(kHeadModifiedByItsParsing.substr(cut)), http::StatusCodeOK);
    EXPECT_EQ(snapshot(), expected);
  }
}

TEST_F(HttpRequestViewTest, HeadReceivedByteByByteIsParsedOnce) {
  ASSERT_EQ(reqSet(RawChars(kHeadModifiedByItsParsing)), http::StatusCodeOK);
  const HeadSnapshot expected = snapshot();

  // The server parses a head only once the minimal request line is received: nothing is parsed before.
  inBuffer.clear();
  receive(kHeadModifiedByItsParsing.substr(0, http::kHttpReqLineMinLen - 1));
  for (std::size_t pos = http::kHttpReqLineMinLen - 1; pos + 1 < kHeadModifiedByItsParsing.size(); ++pos) {
    ASSERT_EQ(receiveAndParse(kHeadModifiedByItsParsing.substr(pos, 1)), kNeedMoreData) << pos;
  }
  ASSERT_EQ(receiveAndParse(kHeadModifiedByItsParsing.substr(kHeadModifiedByItsParsing.size() - 1)),
            http::StatusCodeOK);
  EXPECT_EQ(snapshot(), expected);

  // The next request on the connection is parsed from its start.
  inBuffer.clear();
  ASSERT_EQ(receiveAndParse("GET /next?k=v HTTP/1.1\r\nHost: h\r\n\r\n"), http::StatusCodeOK);
  EXPECT_EQ(req.path(), "/next");
  EXPECT_EQ(req.queryParamValueOrEmpty("k"), "v");
  EXPECT_EQ(req.headerValueOrEmpty("x-dup"), "");
}

TEST_F(HttpRequestViewTest, ErrorInTheRestOfAPartialHead) {
  inBuffer.clear();
  ASSERT_EQ(receiveAndParse("GET /?a=1 HTTP/1.1\r\nHost: h\r\n"), kNeedMoreData);
  EXPECT_EQ(receiveAndParse("Bad Header\r\n\r\n"), http::StatusCodeBadRequest);

  inBuffer.clear();
  ASSERT_EQ(receiveAndParse("GET /?a=1 HTTP/1.1\r\nHost: h\r\nX-Long: "), kNeedMoreData);
  EXPECT_EQ(receiveAndParse(std::string(4096, 'v') + "\r\n\r\n"), http::StatusCodeRequestHeaderFieldsTooLarge);

  // A new request after an error is parsed from its start.
  EXPECT_EQ(reqSet(BuildRaw("GET", "/fresh?x=y")), http::StatusCodeOK);
  EXPECT_EQ(req.path(), "/fresh");
  EXPECT_EQ(req.queryParamValueOrEmpty("x"), "y");
}

// A line that can never end with a CRLF is rejected as soon as it is received: waiting for the rest of the head would
// last until its timeout, if any.
TEST_F(HttpRequestViewTest, BareLfInTheLastLineReceivedIsRejectedWithoutWaiting) {
  // No CR at all (a client using LF as line terminator), the head being complete or not.
  for (std::string_view head : {"GET / HTTP/1.1\nHost: h\n\n", "GET / HTTP/1.1\nHost: h\n", "GET /a HTTP/1.1\n",
                                "GET / HTTP/1.0\n\n", "GET / HTTP/1.1\nHost: h\nContent-Length: 4\n\nabcd"}) {
    SCOPED_TRACE(head);
    EXPECT_EQ(receiveAndParse(head), http::StatusCodeBadRequest);
    inBuffer.clear();
  }

  // The request line is terminated by a CRLF, the last lines are not.
  for (std::string_view head : {"GET / HTTP/1.1\r\nHost: h\n\n", "GET / HTTP/1.1\r\nHost: h\r\nX: v\n",
                                "GET / HTTP/1.1\r\nHost: h\r\n\n", "GET / HTTP/1.1\r\nHost: h\r\nX: v\nY: w\n\n"}) {
    SCOPED_TRACE(head);
    EXPECT_EQ(receiveAndParse(head), http::StatusCodeBadRequest);
    inBuffer.clear();
  }

  // A bare LF in a line a CRLF terminates later is rejected as well.
  for (std::string_view head : {"GET / HTTP/1.1\nHost: h\r\n\r\n", "GET /a\nb HTTP/1.1\r\nHost: h\r\n\r\n",
                                "GET / HTTP/1.1\r\nHost: h\nX: v\r\n\r\n"}) {
    SCOPED_TRACE(head);
    EXPECT_EQ(receiveAndParse(head), http::StatusCodeBadRequest);
    inBuffer.clear();
  }

  // The CR of a line terminator still to be followed by its LF, or no terminator yet, is waited for.
  struct Split {
    std::string_view received;
    std::string_view rest;
  };
  for (const auto& [received, rest] :
       {Split{"GET /ab HTTP/1.1", "\r\nHost: h\r\n\r\n"}, Split{"GET /ab HTTP/1.1\r", "\nHost: h\r\n\r\n"},
        Split{"GET / HTTP/1.1\r\nHo", "st: h\r\n\r\n"}, Split{"GET / HTTP/1.1\r\nHost: h\r", "\n\r\n"},
        Split{"GET / HTTP/1.1\r\nHost: h\r\nX: v", "\r\n\r\n"}}) {
    SCOPED_TRACE(received);
    ASSERT_EQ(receiveAndParse(received), kNeedMoreData);
    EXPECT_EQ(receiveAndParse(rest), http::StatusCodeOK);
    inBuffer.clear();
  }
}

// The lines already parsed by a previous read are not searched again, only the one being received: a bare LF there
// is rejected whatever its size.
TEST_F(HttpRequestViewTest, BareLfInTheRestOfAPartialHeadIsRejected) {
  inBuffer.clear();
  ASSERT_EQ(receiveAndParse("GET /?a=1 HTTP/1.1\r\nHost: h\r\nX-Part"), kNeedMoreData);
  EXPECT_EQ(receiveAndParse("ial: v\nY: w\n\n"), http::StatusCodeBadRequest);

  inBuffer.clear();
  ASSERT_EQ(receiveAndParse("GET /?a=1 HTTP/1.1\r\nHost: h\r\n"), kNeedMoreData);
  EXPECT_EQ(receiveAndParse("\n"), http::StatusCodeBadRequest);

  inBuffer.clear();
  ASSERT_EQ(receiveAndParse("GET /?a=1 HTTP/1.1\r\nHost: h\r\nX: "), kNeedMoreData);
  ASSERT_EQ(receiveAndParse(std::string(1000, 'v')), kNeedMoreData);
  EXPECT_EQ(receiveAndParse("\n"), http::StatusCodeBadRequest);

  // A CRLF split between two reads is not a bare LF.
  inBuffer.clear();
  ASSERT_EQ(receiveAndParse("GET /?a=1 HTTP/1.1\r\nHost: h\r"), kNeedMoreData);
  ASSERT_EQ(receiveAndParse("\n"), kNeedMoreData);
  EXPECT_EQ(receiveAndParse("\r\n"), http::StatusCodeOK);

  // A new request after an error is parsed from its start.
  EXPECT_EQ(reqSet(BuildRaw("GET", "/fresh")), http::StatusCodeOK);
  EXPECT_EQ(req.path(), "/fresh");
}

// RFC 9112 §3: a request-target longer than the server wishes to parse is answered 414. The request line, with the
// empty line that ends the head, is bounded by the size limit of the head (4096 bytes here), as the header lines are
// (431).
TEST_F(HttpRequestViewTest, RequestLineLongerThanTheHeadLimitIsUriTooLong) {
  static constexpr std::size_t kHeadLimit = 4096;
  static constexpr std::string_view kBeforeTarget = "GET ";
  static constexpr std::string_view kAfterTarget = " HTTP/1.0\r\n";  // no Host header field required
  static constexpr std::size_t kMaxTargetSize =
      kHeadLimit - kBeforeTarget.size() - kAfterTarget.size() - http::CRLF.size();

  const auto makeHead = [](std::size_t targetSize, std::string_view rest) {
    std::string head(kBeforeTarget);
    head.push_back('/');
    head.append(targetSize - 1U, 'a');
    head.append(kAfterTarget).append(rest);
    return RawChars(std::string_view(head));
  };

  // The largest request line accepted takes the whole limit, with the empty line.
  ASSERT_EQ(reqSet(makeHead(kMaxTargetSize, http::CRLF)), http::StatusCodeOK);
  EXPECT_EQ(req.path().size(), kMaxTargetSize);

  // One byte more, whatever follows the request line.
  for (std::string_view rest : {"", "\r\n", "Host: h\r\n\r\n", "X-Not-Parsed: v\r\n"}) {
    SCOPED_TRACE(rest);
    EXPECT_EQ(reqSet(makeHead(kMaxTargetSize + 1U, rest)), http::StatusCodeURITooLong);
  }
  EXPECT_EQ(reqSet(makeHead(3U * kHeadLimit, "Host: h\r\n\r\n")), http::StatusCodeURITooLong);

  // The header lines remain bounded with 431, the request line fitting the limit or not.
  EXPECT_EQ(reqSet(makeHead(kMaxTargetSize, "X: v\r\n\r\n")), http::StatusCodeRequestHeaderFieldsTooLarge);
  EXPECT_EQ(reqSet(makeHead(kMaxTargetSize - 100U, std::string(200, 'x').append(": v\r\n\r\n"))),
            http::StatusCodeRequestHeaderFieldsTooLarge);

  // An absolute-form request-target is bounded as well.
  std::string absolute("GET http://example.com/");
  absolute.append(kHeadLimit, 'a').append(" HTTP/1.0\r\n\r\n");
  EXPECT_EQ(reqSet(RawChars(std::string_view(absolute))), http::StatusCodeURITooLong);

  // A new request after an error is parsed from its start.
  EXPECT_EQ(reqSet(BuildRaw("GET", "/fresh")), http::StatusCodeOK);
  EXPECT_EQ(req.path(), "/fresh");
}

TEST_F(HttpRequestViewTest, HeadWaitingForBodyIsNotParsedAgain) {
  static constexpr std::string_view kPostHead =
      "POST /upload?id=7&name=a+b HTTP/1.1\r\nHost: h\r\nX-Dup: v1\r\nX-Dup: v2\r\nContent-Length: 12\r\n\r\n";
  inBuffer.clear();
  ASSERT_EQ(receiveAndParse(std::string(kPostHead) + "partial"), http::StatusCodeOK);
  const HeadSnapshot expected = snapshot();
  EXPECT_FALSE(isHeadWaitingForBody());
  keepHeadForBody();
  EXPECT_TRUE(isHeadWaitingForBody());

  // The rest of the body moves the buffer: the request is resumed without parsing its head again.
  receive(" body");
  resumeHeadWaitingForBody();
  EXPECT_FALSE(isHeadWaitingForBody());
  EXPECT_EQ(snapshot(), expected);
  EXPECT_EQ(req.path(), "/upload");
  EXPECT_EQ(req.queryParamValueOrEmpty("name"), "a b");
  EXPECT_EQ(req.headerValueOrEmpty("x-dup"), "v1,v2");
  EXPECT_EQ(std::string_view(inBuffer).substr(req.headSpanSize()), "partial body");

  // Resuming without a buffer move keeps the views.
  keepHeadForBody();
  resumeHeadWaitingForBody();
  EXPECT_EQ(snapshot(), expected);
}

// The authority of an absolute-form request-target is kept while the head is received in several parts, and moves with
// the buffer.
TEST_F(HttpRequestViewTest, AbsoluteFormHeadReceivedInSeveralPartsIsParsedOnce) {
  static constexpr std::string_view kHead =
      "GET http://user@example.com:8080/a%2Eb?k=v%20w&flag HTTP/1.1\r\n"
      "Host: ignored\r\n"
      "X-Dup: v1\r\n"
      "X-Dup: v2\r\n"
      "\r\n";
  ASSERT_EQ(reqSet(RawChars(kHead)), http::StatusCodeOK);
  const HeadSnapshot expected = snapshot();
  EXPECT_EQ(expected.path, "/a.b");
  EXPECT_EQ(expected.authority, "example.com:8080");
  EXPECT_EQ(req.headerValueOrEmpty("host"), "example.com:8080");
  EXPECT_EQ(req.queryParamValueOrEmpty("k"), "v w");

  for (std::size_t cut = http::kHttpReqLineMinLen; cut < kHead.size(); ++cut) {
    SCOPED_TRACE(cut);
    inBuffer.clear();
    ASSERT_EQ(receiveAndParse(kHead.substr(0, cut)), kNeedMoreData);
    ASSERT_EQ(receiveAndParse(kHead.substr(cut)), http::StatusCodeOK);
    EXPECT_EQ(snapshot(), expected);
  }

  // One byte at a time, the buffer moving each time.
  inBuffer.clear();
  receive(kHead.substr(0, http::kHttpReqLineMinLen - 1));
  for (std::size_t pos = http::kHttpReqLineMinLen - 1; pos + 1 < kHead.size(); ++pos) {
    ASSERT_EQ(receiveAndParse(kHead.substr(pos, 1)), kNeedMoreData) << pos;
  }
  ASSERT_EQ(receiveAndParse(kHead.substr(kHead.size() - 1)), http::StatusCodeOK);
  EXPECT_EQ(snapshot(), expected);

  // The next request on the connection, in origin-form, is parsed from its start.
  inBuffer.clear();
  ASSERT_EQ(receiveAndParse("GET /next HTTP/1.1\r\nHost: next\r\n\r\n"), http::StatusCodeOK);
  EXPECT_EQ(req.path(), "/next");
  EXPECT_TRUE(req.authority().empty());
  EXPECT_EQ(req.headerValueOrEmpty("host"), "next");
}

TEST_F(HttpRequestViewTest, AbsoluteFormHeadWaitingForBodyIsNotParsedAgain) {
  static constexpr std::string_view kPostHead =
      "POST http://h.example:81/upload?id=7 HTTP/1.1\r\nHost: x\r\nContent-Length: 12\r\n\r\n";
  inBuffer.clear();
  ASSERT_EQ(receiveAndParse(std::string(kPostHead) + "partial"), http::StatusCodeOK);
  const HeadSnapshot expected = snapshot();
  EXPECT_EQ(expected.authority, "h.example:81");
  keepHeadForBody();

  // The rest of the body moves the buffer: the request is resumed without parsing its head again.
  receive(" body");
  resumeHeadWaitingForBody();
  EXPECT_EQ(snapshot(), expected);
  EXPECT_EQ(req.path(), "/upload");
  EXPECT_EQ(req.authority(), "h.example:81");
  EXPECT_EQ(req.headerValueOrEmpty("host"), "h.example:81");
  EXPECT_EQ(req.queryParamValueOrEmpty("id"), "7");
  EXPECT_EQ(std::string_view(inBuffer).substr(req.headSpanSize()), "partial body");
}

TEST_F(HttpRequestViewTest, Http11WithoutHostShouldBeRejected) {
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\nX-Header: value\r\n\r\n")), http::StatusCodeOK);
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.1\r\nX-Header: value\r\n\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.1\r\nX-Header: value\r\nhost: test\r\n\r\n")), http::StatusCodeOK);
}

// RFC 9110 §6.2: a higher HTTP/1 minor version is accepted and processed as HTTP/1.1, the highest version the server
// conforms to - Host is required, keep-alive is the default and 100-continue is honored.
TEST_F(HttpRequestViewTest, HigherHttp1MinorVersionIsProcessedAsHttp11) {
  for (const std::string_view version : {"HTTP/1.2", "HTTP/1.3", "HTTP/1.9"}) {
    SCOPED_TRACE(version);
    ASSERT_EQ(reqSet(BuildRaw("GET", "/p", version, MakeHttp1HeaderLine(http::Expect, "100-continue"))),
              http::StatusCodeOK);
    EXPECT_EQ(req.version(), http::HTTP_1_1);
    EXPECT_TRUE(req.hasExpectContinue());
    EXPECT_TRUE(callClientKeepsAliveForHttp1());

    RawChars noHost("GET /p ");
    noHost.append(version);
    noHost.append("\r\nX-Header: value\r\n\r\n");
    EXPECT_EQ(reqSet(std::move(noHost)), http::StatusCodeBadRequest);
  }
}

// RFC 9112 §2.3: HTTP-version = HTTP-name "/" DIGIT "." DIGIT, HTTP-name being the case-sensitive "HTTP". A well-formed
// version of another major version is not supported (505), any other version token is malformed (400).
TEST_F(HttpRequestViewTest, VersionOtherThanHttp1IsRejected) {
  struct VersionAndStatus {
    std::string_view version;
    http::StatusCode status;
  };
  for (const auto [version, status] : {
           VersionAndStatus{"HTTP/2.0", http::StatusCodeHTTPVersionNotSupported},
           VersionAndStatus{"HTTP/0.9", http::StatusCodeHTTPVersionNotSupported},
           VersionAndStatus{"HTTP/9.9", http::StatusCodeHTTPVersionNotSupported},
           VersionAndStatus{"HTTP/0.0", http::StatusCodeBadRequest},
           VersionAndStatus{"XTTP/1.1", http::StatusCodeBadRequest},
           VersionAndStatus{"http/1.1", http::StatusCodeBadRequest},
           VersionAndStatus{"HTTP-1.1", http::StatusCodeBadRequest},
           VersionAndStatus{"XTTP/2.0", http::StatusCodeBadRequest},
           VersionAndStatus{"http/2.0", http::StatusCodeBadRequest},
           VersionAndStatus{"HTTP/2x0", http::StatusCodeBadRequest},
           VersionAndStatus{"HTTP/1x1", http::StatusCodeBadRequest},
           VersionAndStatus{"HTTP/1.x", http::StatusCodeBadRequest},
           VersionAndStatus{"HTTP/1./", http::StatusCodeBadRequest},
           VersionAndStatus{"HTTP/1.:", http::StatusCodeBadRequest},
           VersionAndStatus{"HTTP/x.1", http::StatusCodeBadRequest},
           VersionAndStatus{"HTTP/1.10", http::StatusCodeBadRequest},
           VersionAndStatus{"HTTP/10.1", http::StatusCodeBadRequest},
       }) {
    SCOPED_TRACE(version);
    RawChars raw("GET /p ");
    raw.append(version);
    raw.append("\r\nHost: h\r\n\r\n");
    EXPECT_EQ(reqSet(std::move(raw)), status);
  }
}

TEST_F(HttpRequestViewTest, InvalidHeaderKey) {
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\n:value\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\n  :value\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\nHeaderKey :value\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\n\tHeaderKey:value\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\nHeader\x7FKey:value\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\nHeader{Key}:value\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\nHeader\x01Key:value\r\n")), http::StatusCodeBadRequest);
}

TEST_F(HttpRequestViewTest, InvalidHeaderValue) {
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\nKey:Value\x7F\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\nKey:Value\x01\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\nKey:Value\r\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\nKey:Val\nue\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\nKey:caf\xC3\xA9\x01\r\n")), http::StatusCodeBadRequest);
}

// RFC 9110 §5.5: obs-text (bytes 0x80-0xFF) is allowed in field values, and kept as opaque data.
TEST_F(HttpRequestViewTest, ObsTextHeaderValueIsKeptAsIs) {
  ASSERT_EQ(reqSet(RawChars("GET /test HTTP/1.1\r\nHost: h\r\nX-Test: caf\xC3\xA9\r\n"
                            "Content-Disposition: attachment; filename=\"r\xC3\xA9sum\xC3\xA9.pdf\"\r\n"
                            "X-Latin1:  \xE9t\xE9 \r\nX-Bytes: \x80\xFF\r\n\r\n")),
            http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("x-test"), "caf\xC3\xA9");
  EXPECT_EQ(req.headerValueOrEmpty("content-disposition"), "attachment; filename=\"r\xC3\xA9sum\xC3\xA9.pdf\"");
  EXPECT_EQ(req.headerValueOrEmpty("x-latin1"), "\xE9t\xE9");
  EXPECT_EQ(req.headerValueOrEmpty("x-bytes"), "\x80\xFF");

  // Duplicates are merged like any other value.
  ASSERT_EQ(reqSet(RawChars("GET /test HTTP/1.1\r\nHost: h\r\nX-Test: \xC3\xA9t\xC3\xA9\r\nX-Test: hiver\r\n\r\n")),
            http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("x-test"), "\xC3\xA9t\xC3\xA9,hiver");
}

TEST_F(HttpRequestViewTest, InvalidHeaderKeyValueSeparator) {
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\nKey;Value\r\n")), http::StatusCodeBadRequest);
}

TEST_F(HttpRequestViewTest, ParseBasicPathAndVersion) {
  auto st = reqSet(BuildRaw("GET", "/abc", "HTTP/1.1"));
  EXPECT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.method(), http::Method::GET);
  EXPECT_EQ(req.path(), "/abc");
  EXPECT_EQ(req.version(), http::HTTP_1_1);
  EXPECT_TRUE(req.queryParams().begin() == req.queryParams().end());
}

TEST_F(HttpRequestViewTest, QueryParamsDecodingPlusAndPercent) {
  // a=1+2&b=hello%20world&c=%zz (malformed % sequence left verbatim for c's value)
  auto st = reqSet(BuildRaw("GET", "/p?a=1+2&b=hello%20world&c=%zz"));
  ASSERT_EQ(st, http::StatusCodeOK);
  vector<http::HeaderView> seen;
  for (auto [key, val] : req.queryParamsRange()) {
    seen.emplace_back(key, val);
  }
  ASSERT_EQ(seen.size(), 3U);
  EXPECT_EQ(seen[0].name, "a");
  EXPECT_EQ(seen[0].value, "1 2");  // '+' => space
  EXPECT_EQ(seen[1].name, "b");
  EXPECT_EQ(seen[1].value, "hello world");  // %20 decoded
  EXPECT_EQ(seen[2].name, "c");
  EXPECT_EQ(seen[2].value, "%zz");  // invalid escape left as-is
}

TEST_F(HttpRequestViewTest, QueryParamInt) {
  auto st = reqSet(BuildRaw("GET", "/p?num=42&str=hello&almost=123abc"));
  ASSERT_EQ(st, http::StatusCodeOK);
  auto valOpt = req.queryParamInt("num");
  ASSERT_TRUE(valOpt.has_value());
  EXPECT_EQ(valOpt.value_or(0), 42);

  valOpt = req.queryParamInt("str");
  EXPECT_FALSE(valOpt.has_value());

  valOpt = req.queryParamInt("almost");
  EXPECT_FALSE(valOpt.has_value());

  EXPECT_FALSE(req.queryParamInt("missing").has_value());
}

TEST_F(HttpRequestViewTest, EmptyAndMissingValues) {
  auto st = reqSet(BuildRaw("GET", "/p?k1=&k2&=v"));
  ASSERT_EQ(st, http::StatusCodeOK);
  vector<http::HeaderView> seen;
  for (auto [key, value] : req.queryParamsRange()) {
    seen.emplace_back(key, value);
  }
  ASSERT_EQ(seen.size(), 3U);
  EXPECT_EQ(seen[0].name, "k1");
  EXPECT_EQ(seen[0].value, "");
  EXPECT_EQ(seen[1].name, "k2");
  EXPECT_EQ(seen[1].value, "");
  EXPECT_EQ(seen[2].name, "");
  EXPECT_EQ(seen[2].value, "v");

  EXPECT_EQ(req.queryParams().size(), 3U);
  EXPECT_EQ(req.queryParams().at("k1"), "");
  EXPECT_EQ(req.queryParams().at("k2"), "");
  EXPECT_EQ(req.queryParams().at(""), "v");  // last occurrence retained
}

TEST_F(HttpRequestViewTest, QueryParamsRangeDuplicateKeysPreservedOrder) {
  auto st = reqSet(BuildRaw("GET", "/p?x=1&x=2&x=3"));
  ASSERT_EQ(st, http::StatusCodeOK);
  vector<std::string_view> values;
  for (auto [key, value] : req.queryParamsRange()) {
    if (key == "x") {
      values.push_back(value);
    }
  }
  ASSERT_EQ(values.size(), 3U);
  EXPECT_EQ(values[0], "1");
  EXPECT_EQ(values[1], "2");
  EXPECT_EQ(values[2], "3");

  EXPECT_EQ(req.queryParams().size(), 1U);
  EXPECT_EQ(req.queryParams().at("x"), "3");  // last occurrence retained in map view

  auto it = req.queryParamsRange().begin();
  EXPECT_EQ((*it).key, "x");
  EXPECT_EQ((*it).value, "1");
  EXPECT_EQ((*it++).key, "x");
  EXPECT_EQ((*it).value, "2");
  EXPECT_EQ((*++it).key, "x");
  EXPECT_EQ((*it).value, "3");

  // iterator should be default constructible
  decltype(req.queryParamsRange().begin()) defIt;
  EXPECT_TRUE(defIt == decltype(req.queryParamsRange().begin()){});
}

// Decoded query parameters are internally delimited with url::kNewPairSep / url::kNewKeyValueSep: these bytes must not
// be produced by the decoding, otherwise a single parameter could be read as several.
TEST_F(HttpRequestViewTest, QueryParamsCannotInjectSeparators) {
  auto st = reqSet(BuildRaw("GET", "/p?a=1%00admin%1Ftrue&b%1Fc%00d=2"));
  ASSERT_EQ(st, http::StatusCodeOK);
  vector<http::HeaderView> seen;
  for (auto [key, val] : req.queryParamsRange()) {
    seen.emplace_back(key, val);
  }
  ASSERT_EQ(seen.size(), 2U);
  EXPECT_EQ(seen[0].name, "a");
  EXPECT_EQ(seen[0].value, "1%00admin%1Ftrue");
  EXPECT_EQ(seen[1].name, "b%1Fc%00d");
  EXPECT_EQ(seen[1].value, "2");
  EXPECT_FALSE(req.queryParams().contains("admin"));
}

TEST_F(HttpRequestViewTest, InvalidPathEscapeCauses400) {
  auto st = reqSet(BuildRaw("GET", "/bad%zz"));
  EXPECT_EQ(st, http::StatusCodeBadRequest);
}

TEST_F(HttpRequestViewTest, HeaderAccessorsBasicAndEmptyVsMissing) {
  // Provide headers including:
  //  - normal value (X-Test)
  //  - empty value (X-Empty)
  //  - value with trailing spaces (X-Trim)
  //  - value with leading & trailing mixed whitespace (X-Spaces)
  //  - lowercase reserved key (content-length)
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "X-Empty:\r\n"
                            "X-Trim: value   \r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  // Existing normal header
  EXPECT_EQ(req.headerValueOrEmpty("x-test"), "Value");
  EXPECT_EQ(req.headerValue("x-test").value_or(""), "Value");

  // Case-insensitive lookup
  EXPECT_EQ(req.headerValueOrEmpty("x-test"), "Value");
  EXPECT_TRUE(req.headerValue("x-test").has_value());

  // Empty header value vs missing header
  EXPECT_EQ(req.headerValueOrEmpty("x-empty"), "");
  EXPECT_TRUE(req.headerValue("x-empty").has_value());

  // Trimming behavior (trailing)
  EXPECT_EQ(req.headerValueOrEmpty("x-trim"), "value");
  // Trimming behavior (leading & trailing)
  EXPECT_EQ(req.headerValueOrEmpty("x-spaces"), "abc");
  EXPECT_EQ(req.headerValue("x-spaces").value_or(""), "abc");

  EXPECT_EQ(req.headerValueOrEmpty("no-such"), std::string_view());
  EXPECT_FALSE(req.headerValue("no-such").has_value());
}

TEST_F(HttpRequestViewTest, HeaderAccessorsAbsentHeaders) {
  auto st = reqSet(BuildRaw("GET", "/p"));
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("host"), "h");  // baseline sanity
  EXPECT_EQ(req.headerValueOrEmpty("x-unknown"), std::string_view());
  EXPECT_FALSE(req.headerValue("x-unknown").has_value());
}

TEST_F(HttpRequestViewTest, MergeConsecutiveHeaders) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H:v1\r\n"
                            "H:v2\r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"x-test", "Value"}, {"h", "v1,v2"}, {"x-spaces", "abc"}, {http::ContentLength, "0"}});
}

TEST_F(HttpRequestViewTest, ContentEncodingValueIsLowerCased) {
  // Content codings are case-insensitive: they are lower-cased when parsed (including merged repeated lines, and
  // values long enough for the vectorized path), whereas other header values keep their case.
  auto st = reqSet(BuildRaw("POST", "/p", "HTTP/1.1",
                            "X-Test: MiXeD\r\n"
                            "Content-Encoding:  GZip, IDENTITY, Deflate, ZSTD, Br, identity \r\n"
                            "X-Other: VaLuE\r\n"
                            "CONTENT-ENCODING: BR\r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({
      {"x-test", "MiXeD"},
      {http::ContentEncoding, "gzip, identity, deflate, zstd, br, identity,br"},
      {"x-other", "VaLuE"},
  });
}

TEST_F(HttpRequestViewTest, ShrinkToFit) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "X-Test: Value\r\n"
                      "Cookie:  cookie1 \r\n"
                      "X-Spaces:    abc \t  \r\n"
                      "Cookie:\r\n"
                      "Cookie:cookie2\r\n"
                      "Cookie:cookie3\r\n"
                      "X-Spaces:    de \t  \r\n"
                      "content-length: 0\r\n"
                      "X-Spaces:fgh \t  \r\n"
                      "Cookie: cookie4\r\n");

  rehash(1000);

  const auto originalLoadfactor = req.headers().load_factor();
  ASSERT_LT(originalLoadfactor, 0.25F);

  auto st = reqSet(std::move(raw));
  ASSERT_EQ(st, http::StatusCodeOK);

  shrinkAndMaybeClear();

  EXPECT_TRUE(req.headers().empty());
}

TEST_F(HttpRequestViewTest, MergeConsecutiveHeadersWithSpaces) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H: v1  \r\n"
                            "H: v2\r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"x-test", "Value"}, {"h", "v1,v2"}, {"x-spaces", "abc"}, {http::ContentLength, "0"}});
}

TEST_F(HttpRequestViewTest, MergeNonConsecutiveHeaders) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H:v1\r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "H:v2\r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"x-test", "Value"}, {"h", "v1,v2"}, {"x-spaces", "abc"}, {http::ContentLength, "0"}});
}

TEST_F(HttpRequestViewTest, MergeNonConsecutiveHeadersWithSpaces) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H: v1  \r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "H: v2\r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"x-test", "Value"}, {"h", "v1,v2"}, {"x-spaces", "abc"}, {http::ContentLength, "0"}});
}

TEST_F(HttpRequestViewTest, MergeNonConsecutiveHeadersWithEmptyOnFirst) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H:  \r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "H:v2\r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"x-test", "Value"}, {"h", "v2"}, {"x-spaces", "abc"}, {http::ContentLength, "0"}});
}

TEST_F(HttpRequestViewTest, MergeNonConsecutiveHeadersWithEmptyOnSecond) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H: v1  \r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "H:\r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"x-test", "Value"}, {"h", "v1"}, {"x-spaces", "abc"}, {http::ContentLength, "0"}});
}

TEST_F(HttpRequestViewTest, MergeNonConsecutiveHeadersBothEmpty) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H:   \r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "H:\r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"x-test", "Value"}, {"h", ""}, {"x-spaces", "abc"}, {http::ContentLength, "0"}});

  EXPECT_TRUE(req.headerValue("h").has_value());
}

TEST_F(HttpRequestViewTest, MergeMultipleCookies) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "Cookie:  cookie1 \r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "Cookie:\r\n"
                            "Cookie:cookie2\r\n"
                            "Cookie:cookie3\r\n"
                            "content-length: 0\r\n"
                            "Cookie: cookie4\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({
      {"x-test", "Value"},
      {"cookie", "cookie1;cookie2;cookie3;cookie4"},
      {"x-spaces", "abc"},
      {http::ContentLength, "0"},
  });
}

TEST_F(HttpRequestViewTest, MergeMultipleHeaders) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "X-Test: Value\r\n"
                      "Cookie:  cookie1 \r\n"
                      "X-Spaces:    abc \t  \r\n"
                      "Cookie:\r\n"
                      "Cookie:cookie2\r\n"
                      "Cookie:cookie3\r\n"
                      "X-Spaces:    de \t  \r\n"
                      "content-length: 0\r\n"
                      "X-Spaces:fgh \t  \r\n"
                      "Cookie: cookie4\r\n");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({
      {"x-test", "Value"},
      {"cookie", "cookie1;cookie2;cookie3;cookie4"},
      {"x-spaces", "abc,de,fgh"},
      {http::ContentLength, "0"},
  });

  // merge not allowed for custom header X-Spaces
  st = reqSet(std::move(raw), false);
  ASSERT_EQ(st, http::StatusCodeBadRequest);
}

TEST_F(HttpRequestViewTest, AcceptHeaderCommaMerge) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "Accept: text/plain\r\n"
                            "Accept: text/html\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("accept"), "text/plain,text/html");
  EXPECT_TRUE(req.hasHeader("accept"));
  EXPECT_FALSE(req.hasHeader("bccept"));
}

TEST_F(HttpRequestViewTest, AcceptHeaderSkipEmptySecond) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "Accept: text/plain\r\n"
                            "Accept:   \r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("accept"), "text/plain");
}

TEST_F(HttpRequestViewTest, AcceptHeaderEmptyFirstTakesSecond) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "Accept:    \r\n"
                            "Accept: text/html\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("accept"), "text/html");
}

TEST_F(HttpRequestViewTest, UserAgentSpaceMerge) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "User-Agent: Foo  \r\n"
                            "User-Agent:   Bar   \r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("user-agent"), "Foo Bar");
}

TEST_F(HttpRequestViewTest, DuplicateAuthorizationIsRejectedWhateverTheCaseOfItsName) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "aUtHoRiZaTiOn: Bearer First\r\n"
                            "AUTHORIZATION: Bearer Second\r\n"));
  EXPECT_EQ(st, http::StatusCodeBadRequest);
}

TEST_F(HttpRequestViewTest, HasMoreBodyReturnsFalseWhenAggregated) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  setBodyAccessAggregated();
  EXPECT_FALSE(req.hasMoreBody());
}

TEST_F(HttpRequestViewTest, HasMoreBodyReturnsFalseWhenBridgeHasNoHasMore) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  setBodyAccessStreamingWithBridgeNoHasMore();
  EXPECT_FALSE(req.hasMoreBody());
}

TEST_F(HttpRequestViewTest, BodyAfterReadBodyThrows) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  // Simulate streaming read first
  installStreamingBridge();

  // call readBody -> switches to Streaming
  auto chunk = req.readBody();
  EXPECT_EQ(chunk, "chunk-data");

  // calling body() after readBody() should throw
  EXPECT_THROW({ (void)req.body(); }, std::logic_error);
}

TEST_F(HttpRequestViewTest, ReadBodyAfterBodyThrows) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  // Simulate aggregate bridge
  installAggregateBridge();

  // call body() -> switches to Aggregated
  auto full = req.body();
  EXPECT_EQ(full, "full-body");

  // calling readBody after body() should throw
  EXPECT_THROW({ (void)req.readBody(); }, std::logic_error);
}

TEST_F(HttpRequestViewTest, HasMoreBodyShouldBeFalseByDefault) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  EXPECT_FALSE(req.hasMoreBody());
}

TEST_F(HttpRequestViewTest, Http2FieldsShouldBeFilledCorrectlyInHttp1) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

#ifdef AERONET_ENABLE_HTTP2
  EXPECT_FALSE(req.isHttp2());
  EXPECT_EQ(req.streamId(), 0);
  EXPECT_TRUE(req.scheme().empty());
  EXPECT_TRUE(req.authority().empty());
#endif
}

TEST_F(HttpRequestViewTest, ReadBodyWithBridgeReturnsChunk) {
  const auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  installStreamingBridge();

  auto chunk = req.readBody(64);
  EXPECT_EQ(chunk, "chunk-data");

  EXPECT_TRUE(req.isBodyReady());
}

TEST_F(HttpRequestViewTest, HasMoreBodyWithBridgeTrue) {
  const auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  installStreamingBridge();

  EXPECT_TRUE(req.hasMoreBody());
}

TEST_F(HttpRequestViewTest, BodyWithAggregateBridgeReturnsFullBody) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  installAggregateBridge();

  auto full = req.body();
  EXPECT_EQ(full, "full-body");

  EXPECT_TRUE(req.isBodyReady());
}

TEST_F(HttpRequestViewTest, BodyShouldBeReadyIfBodyCalled) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  EXPECT_FALSE(req.isBodyReady());
  [[maybe_unused]] auto full = req.body();
  EXPECT_TRUE(req.isBodyReady());
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS

TEST_F(HttpRequestViewTest, PinHeadStorageRemapsViews) {
  // Build raw request with a header value we can inspect pointer for
  RawChars raw = BuildRaw("GET", "/p", "HTTP/1.1", "X-Custom: original_value\r\n");
  auto st = reqSet(std::move(raw));
  ASSERT_EQ(st, http::StatusCodeOK);

  // capture original view pointer into connection inBuffer
  auto hostView = req.headerValueOrEmpty("x-custom");
  const char* originalPtr = hostView.data();

  // now pin head storage which moves data into state.asyncState->headBuffer
  callPinHeadStorage();

  // after pinning, header view should point into headBuffer (ownerState.headBuffer)
  auto pinned = req.headerValueOrEmpty("x-custom");
  const char* pinnedPtr = pinned.data();

  ASSERT_NE(originalPtr, pinnedPtr);
  // pinned pointer should be within headBuffer data range
  const char* hb = headBuffer.data();
  EXPECT_GE(pinnedPtr, hb);
  EXPECT_LT(pinnedPtr, hb + static_cast<std::ptrdiff_t>(headBuffer.size()));
}

TEST_F(HttpRequestViewTest, PinHead_SkipsRemapForViewsBeyondOldLimit) {
  // Build a simple request to populate head span
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1", "X-A: a\r\n");
  auto st = reqSet(std::move(raw));
  ASSERT_EQ(st, http::StatusCodeOK);

  // Ensure there is extra data in the connection buffer after the head span
  const char* extra = "EXTRA_PAYLOAD_DATA";
  inBuffer.append(extra, static_cast<std::size_t>(std::strlen(extra)));

  // Create a header whose value points to bytes beyond the head span (>= oldLimit)
  const char* oldBase = inBuffer.data();
  const char* beyond = oldBase + static_cast<std::ptrdiff_t>(req.headSpanSize()) + 2;
  // Use fixture helper to add header view pointing into connection buffer beyond oldLimit
  setHeaderViewToPtr("x-outside", beyond, 5);

  // Record pointer before pinning
  auto before = req.headerValueOrEmpty("x-outside");
  const char* beforePtr = before.data();

  // Call pin which should NOT remap the view (it lies beyond oldLimit)
  callPinHeadStorage();

  auto after = req.headerValueOrEmpty("x-outside");
  const char* afterPtr = after.data();

  EXPECT_EQ(beforePtr, afterPtr);

  // Ensure the after pointer still points into connection inBuffer (not into headBuffer)
  const char* inBase = inBuffer.data();
  EXPECT_GE(afterPtr, inBase);
  EXPECT_LT(afterPtr, inBase + static_cast<std::ptrdiff_t>(inBuffer.size()));
  // And ensure it's not inside headBuffer
  const char* hb = headBuffer.data();
  EXPECT_FALSE(afterPtr >= hb && afterPtr < hb + static_cast<std::ptrdiff_t>(headBuffer.size()));
}

TEST_F(HttpRequestViewTest, PinHead_SkipsRemapForViewsBeforeOldBase) {
  // Build a simple request to populate head span
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1", "X-B: b\r\n");
  auto st = reqSet(std::move(raw));
  ASSERT_EQ(st, http::StatusCodeOK);

  // Allocate a temporary small buffer and make a pointer that points before oldBase
  std::string tmp = "PRE_DATA";
  const char* tmpPtr = tmp.data();
  // choose a pointer that lies before oldBase
  const char* before = tmpPtr;  // tmp is separate from inBuffer

  // Add header view that points before oldBase
  setHeaderViewToPtr("x-before", before, 3);

  auto beforeView = req.headerValueOrEmpty("x-before");
  const char* beforePtr = beforeView.data();

  callPinHeadStorage();

  auto afterView = req.headerValueOrEmpty("x-before");
  const char* afterPtr = afterView.data();

  // Pointer should be unchanged (remap skipped)
  EXPECT_EQ(beforePtr, afterPtr);

  // Ensure it still points into tmp buffer (not into headBuffer)
  const char* hb = headBuffer.data();
  EXPECT_FALSE(afterPtr >= hb && afterPtr < hb + static_cast<std::ptrdiff_t>(headBuffer.size()));
}

TEST_F(HttpRequestViewTest, PinHead_RemapsEntriesInsideOldSpan) {
  // Build request with headers so head span contains header values we can reference
  RawChars raw = BuildRaw("GET", "/p", "HTTP/1.1", "X-Remap: val\r\n");
  auto st = reqSet(std::move(raw));
  ASSERT_EQ(st, http::StatusCodeOK);

  // Capture pointer into the current inBuffer for header value
  auto hv = req.headerValueOrEmpty("x-remap");
  const char* origPtr = hv.data();

  // Also set a trailer and a path param value pointing inside the head span by using the same pointer
  setTrailerViewToPtr("t-remap", origPtr, 3);
  setPathParamToPtr("pp", origPtr, 3);

  // Pin head storage which should remap entries that are inside the old span
  callPinHeadStorage();

  // After pinning, header value pointer must have changed (moved into headBuffer)
  auto pinnedHeader = req.headerValueOrEmpty("x-remap");
  const char* pinnedPtr = pinnedHeader.data();
  EXPECT_NE(origPtr, pinnedPtr);

  // Trailer and path param entries should also have been remapped
  auto tr = req.trailers().find("t-remap");
  ASSERT_TRUE(tr != req.trailers().end());
  const char* trPtr = tr->second.data();
  EXPECT_NE(origPtr, trPtr);

  auto ppIt = req.pathParams().find("pp");
  ASSERT_TRUE(ppIt != req.pathParams().end());
  const char* ppPtr = ppIt->second.data();
  EXPECT_NE(origPtr, ppPtr);

  // All remapped pointers should be inside headBuffer
  const char* hb = headBuffer.data();
  EXPECT_GE(pinnedPtr, hb);
  EXPECT_LT(pinnedPtr, hb + static_cast<std::ptrdiff_t>(headBuffer.size()));
  EXPECT_GE(trPtr, hb);
  EXPECT_LT(trPtr, hb + static_cast<std::ptrdiff_t>(headBuffer.size()));
  EXPECT_GE(ppPtr, hb);
  EXPECT_LT(ppPtr, hb + static_cast<std::ptrdiff_t>(headBuffer.size()));
}

#endif

TEST_F(HttpRequestViewTest, ConnectionCloseAndHasExpectContinue) {
  {  // Connection: close
    auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1", MakeHttp1HeaderLine(http::Connection, "close")));
    ASSERT_EQ(st, http::StatusCodeOK);
    EXPECT_FALSE(callClientKeepsAliveForHttp1());
    EXPECT_FALSE(req.hasExpectContinue());
  }
  {  // Expect: 100-continue on HTTP/1.1
    auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1", MakeHttp1HeaderLine(http::Expect, "100-continue")));
    ASSERT_EQ(st, http::StatusCodeOK);
    EXPECT_TRUE(callClientKeepsAliveForHttp1());
    EXPECT_TRUE(req.hasExpectContinue());
  }
  {  // Expect header on HTTP/1.0 should be ignored
    auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.0", MakeHttp1HeaderLine(http::Expect, "100-continue")));
    ASSERT_EQ(st, http::StatusCodeOK);
    EXPECT_FALSE(req.hasExpectContinue());
  }
}

// RFC 9110 §7.6.1: Connection is a list of connection options, so the close option is honored wherever it appears in
// the list - including when repeated Connection header lines are merged into a single value.
TEST_F(HttpRequestViewTest, ConnectionCloseOptionAnywhereInList) {
  for (std::string_view headers : {
           "Connection: keep-alive, close\r\n",
           "Connection: Upgrade,CLOSE\r\n",
           "Connection: close\r\nConnection: close\r\n",
           "Connection: keep-alive\r\nConnection:  Close \r\n",
           "Connection: ,close,\r\n",
       }) {
    SCOPED_TRACE(headers);
    ASSERT_EQ(reqSet(BuildRaw("GET", "/p", "HTTP/1.1", headers)), http::StatusCodeOK);
    EXPECT_FALSE(callClientKeepsAliveForHttp1());
  }
  // Options merely containing "close" are distinct options
  for (std::string_view headers :
       {"Connection: closed\r\n", "Connection: not-close\r\n", "Connection: Upgrade\r\n", "Connection: ,\r\n"}) {
    SCOPED_TRACE(headers);
    ASSERT_EQ(reqSet(BuildRaw("GET", "/p", "HTTP/1.1", headers)), http::StatusCodeOK);
    EXPECT_TRUE(callClientKeepsAliveForHttp1());
  }
}

// RFC 9112 §9.3: an HTTP/1.0 connection only persists when the request carries the keep-alive connection option.
TEST_F(HttpRequestViewTest, Http10KeepAliveRequiresKeepAliveOption) {
  struct Case {
    std::string_view headers;
    bool keepAlive;
  };
  for (const Case& tc : {
           Case{"", false},
           Case{"Connection: Upgrade\r\n", false},
           Case{"Connection: keep-alive\r\n", true},
           Case{"Connection: Upgrade, Keep-Alive\r\n", true},
           Case{"Connection: keep-alive\r\nConnection: close\r\n", false},
       }) {
    SCOPED_TRACE(tc.headers);
    ASSERT_EQ(reqSet(BuildRaw("GET", "/p", "HTTP/1.0", tc.headers)), http::StatusCodeOK);
    EXPECT_EQ(callClientKeepsAliveForHttp1(), tc.keepAlive);
  }
}

TEST_F(HttpRequestViewTest, EndSetsSpanAttributesAndEnds) {
  // Reset fake span static state
  FakeSpan::lastStatusCode = -1;
  FakeSpan::lastDurationUs = -1;
  FakeSpan::ended = false;

  auto spanPtr = std::make_unique<FakeSpan>();
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1", "");
  auto st = reqSet(std::move(raw), true);
  ASSERT_EQ(st, http::StatusCodeOK);

  // attach fake span to request
  attachSpan(std::move(spanPtr));

  // call end with specific status code
  callEnd(http::StatusCodeNotFound);

  EXPECT_EQ(FakeSpan::lastStatusCode, static_cast<int64_t>(http::StatusCodeNotFound));
  EXPECT_GT(FakeSpan::lastDurationUs, -1);
  EXPECT_TRUE(FakeSpan::ended);
}

TEST_F(HttpRequestViewTest, DuplicateSingletonHeadersProduce400) {
  // A proxy in front may keep the first occurrence where we would keep the last one: they are rejected.
  for (std::string_view name : {"authorization", "proxy-authorization", "content-type", "range", "if-range",
                                "if-modified-since", "if-unmodified-since", "referer", "from", "max-forwards"}) {
    SCOPED_TRACE(name);
    std::string first(name);
    first.append(": bytes=0-99\r\n");
    std::string second(name);
    second.append(": bytes=100-199\r\n");
    EXPECT_EQ(reqSet(BuildRaw("GET", "/p", "HTTP/1.1", first + second)), http::StatusCodeBadRequest);

    // Also when the first occurrence is empty, or the second one.
    std::string empty(name);
    empty.append(":\r\n");
    EXPECT_EQ(reqSet(BuildRaw("GET", "/p", "HTTP/1.1", empty + second)), http::StatusCodeBadRequest);
    EXPECT_EQ(reqSet(BuildRaw("GET", "/p", "HTTP/1.1", first + empty)), http::StatusCodeBadRequest);

    // A single occurrence is kept as is.
    EXPECT_EQ(reqSet(BuildRaw("GET", "/p", "HTTP/1.1", second)), http::StatusCodeOK);
    EXPECT_EQ(req.headerValueOrEmpty(LowerAsciiKey{name}), "bytes=100-199");
  }
}

TEST_F(HttpRequestViewTest, DuplicateContentLengthProduces400) {
  auto st = reqSet(BuildRaw("POST", "/p", "HTTP/1.1",
                            "Content-Length: 5\r\n"
                            "Content-Length: 5\r\n"));
  EXPECT_EQ(st, http::StatusCodeBadRequest);
}

TEST_F(HttpRequestViewTest, DuplicateHostProduces400) {
  // BuildRaw already injects one Host header; we append another duplicate -> 400
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1", "Host: other\r\n"));
  EXPECT_EQ(st, http::StatusCodeBadRequest);
}

namespace {

// Deterministic PRNG for reproducibility
class FuzzRng {
 public:
  explicit FuzzRng(uint64_t seed) : _gen(seed) {}

  uint8_t byte() { return static_cast<uint8_t>(_dist(_gen)); }

  uint32_t u32() { return static_cast<uint32_t>(_dist(_gen)) | (static_cast<uint32_t>(_dist(_gen)) << 8U); }

  std::size_t range(std::size_t lo, std::size_t hi) {
    if (lo >= hi) {
      return lo;
    }
    return lo + (u32() % (hi - lo));
  }

  bool coin() { return (byte() & 1U) != 0; }

 private:
  std::mt19937_64 _gen;
  std::uniform_int_distribution<uint16_t> _dist{0, 255};
};

// Generate a random buffer of given size
RawChars RandomBuffer(FuzzRng& rng, std::size_t size) {
  RawChars buf(size);
  char* pData = buf.data();
  for (std::size_t ii = 0; ii < size; ++ii) {
    *pData++ = static_cast<char>(rng.byte());
  }
  buf.setEnd(pData);
  return buf;
}

// Generate semi-valid HTTP request-like data
RawChars SemiValidRequest(FuzzRng& rng) {
  static constexpr std::array kMethods{"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS", "TRACE"};
  static constexpr std::array kVersions{"HTTP/1.0", "HTTP/1.1", "HTTP/2.0", "HTTP/0.9", "HTXP/1.1"};

  RawChars buf;

  // Method
  if (rng.coin()) {
    buf.append(kMethods[rng.range(0, kMethods.size())]);
  } else {
    // Random method-like token
    std::size_t len = rng.range(1, 10);
    for (std::size_t ii = 0; ii < len; ++ii) {
      buf.push_back(static_cast<char>('A' + rng.range(0, 26)));
    }
  }
  buf.push_back(' ');

  // Path
  buf.push_back('/');
  std::size_t pathLen = rng.range(0, 50);
  for (std::size_t ii = 0; ii < pathLen; ++ii) {
    uint8_t choice = static_cast<uint8_t>(rng.range(0, 10));
    if (choice < 5) {
      buf.push_back(static_cast<char>('a' + rng.range(0, 26)));
    } else if (choice < 7) {
      buf.push_back('/');
    } else if (choice < 9) {
      buf.push_back('%');
      buf.push_back(static_cast<char>('0' + rng.range(0, 10)));
      buf.push_back(static_cast<char>('0' + rng.range(0, 10)));
    } else {
      buf.push_back(static_cast<char>(rng.byte()));
    }
  }

  // Query string (sometimes)
  if (rng.coin()) {
    buf.push_back('?');
    std::size_t queryLen = rng.range(0, 30);
    for (std::size_t ii = 0; ii < queryLen; ++ii) {
      uint8_t choice = static_cast<uint8_t>(rng.range(0, 5));
      if (choice == 0) {
        buf.push_back('=');
      } else if (choice == 1) {
        buf.push_back('&');
      } else if (choice == 2) {
        buf.push_back('+');
      } else {
        buf.push_back(static_cast<char>('a' + rng.range(0, 26)));
      }
    }
  }

  buf.push_back(' ');

  // Version
  if (rng.coin()) {
    buf.append(kVersions[rng.range(0, kVersions.size())]);
  } else {
    std::size_t len = rng.range(0, 15);
    for (std::size_t ii = 0; ii < len; ++ii) {
      buf.push_back(static_cast<char>(rng.byte()));
    }
  }
  buf.append(http::CRLF);

  // Headers
  std::size_t numHeaders = rng.range(0, 10);
  for (std::size_t hh = 0; hh < numHeaders; ++hh) {
    // Header name
    std::size_t nameLen = rng.range(1, 20);
    for (std::size_t ii = 0; ii < nameLen; ++ii) {
      if (rng.coin()) {
        buf.push_back(static_cast<char>('A' + rng.range(0, 26)));
      } else {
        buf.push_back(static_cast<char>('a' + rng.range(0, 26)));
      }
    }

    // Separator (sometimes malformed)
    if (rng.range(0, 10) < 8) {
      buf.push_back(':');
      if (rng.coin()) {
        buf.push_back(' ');
      }
    } else {
      // Malformed separator
      buf.push_back(static_cast<char>(rng.byte()));
    }

    // Header value
    std::size_t valLen = rng.range(0, 50);
    for (std::size_t ii = 0; ii < valLen; ++ii) {
      char ch = static_cast<char>(rng.byte());
      // Avoid embedding CRLF in values (unless we want to test header injection)
      if (ch == '\r' || ch == '\n') {
        ch = ' ';
      }
      buf.push_back(ch);
    }
    buf.append(http::CRLF);
  }

  // Terminal CRLF (sometimes missing)
  if (rng.range(0, 10) < 8) {
    buf.append(http::CRLF);
  }

  return buf;
}

}  // namespace

// Fuzz test with purely random bytes
TEST_F(HttpRequestViewTest, RandomBytes) {
  constexpr std::size_t kIterations = 5000;
  constexpr std::size_t kMaxSize = 1024;

  for (std::size_t seed = 0; seed < kIterations; ++seed) {
    FuzzRng rng(seed);
    std::size_t size = rng.range(0, kMaxSize);
    RawChars input = RandomBuffer(rng, size);
    ASSERT_NO_FATAL_FAILURE(fuzzHttpRequestParsing(input));
  }
}

// Fuzz test with semi-valid HTTP request structure
TEST_F(HttpRequestViewTest, SemiValidRequests) {
  constexpr std::size_t kIterations = 5000;

  for (std::size_t seed = 0; seed < kIterations; ++seed) {
    FuzzRng rng(seed + 1000000);  // Different seed range
    RawChars input = SemiValidRequest(rng);
    ASSERT_NO_FATAL_FAILURE(fuzzHttpRequestParsing(input));
  }
}

// Fuzz test with mutation of valid requests
TEST_F(HttpRequestViewTest, MutatedValidRequests) {
  constexpr std::size_t kIterations = 6000;

  // Base valid requests to mutate
  static const std::string_view kBaseRequests[]{
      "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n",
      "POST /api/data HTTP/1.1\r\nHost: api.example.com\r\nContent-Length: 0\r\n\r\n",
      "PUT /resource HTTP/1.0\r\nHost: host\r\nContent-Type: application/json\r\n\r\n",
      "OPTIONS /longer/path HTTP/1.3\r\nHost: another-host\r\nX-Header:some value\r\n\r\n",
      "DELETE /item/123 HTTP/1.1\r\nHost: h\r\nAuthorization: Bearer token\r\n\r\n",
      "GET /path?key=value&foo=bar HTTP/1.1\r\nHost: h\r\nAccept: */*\r\n\r\n",
  };

  for (std::size_t seed = 0; seed < kIterations; ++seed) {
    FuzzRng rng(seed + 2000000);

    // Pick a base request
    std::string_view base = kBaseRequests[rng.range(0, std::size(kBaseRequests))];
    std::string input(base);

    // Apply mutations
    std::size_t numMutations = rng.range(1, 10);
    for (std::size_t mm = 0; mm < numMutations && !input.empty(); ++mm) {
      std::size_t mutationType = rng.range(0, 5);
      std::size_t pos = rng.range(0, input.size());

      switch (mutationType) {
        case 0:  // Flip a byte
          input[pos] = static_cast<char>(static_cast<uint8_t>(input[pos]) ^ rng.byte());
          break;
        case 1:  // Insert random byte
          input.insert(pos, 1, static_cast<char>(rng.byte()));
          break;
        case 2:  // Delete a byte
          if (!input.empty()) {
            input.erase(pos, 1);
          }
          break;
        case 3:  // Replace with random bytes
          if (pos + 3 <= input.size()) {
            input[pos] = static_cast<char>(rng.byte());
            input[pos + 1] = static_cast<char>(rng.byte());
            input[pos + 2] = static_cast<char>(rng.byte());
          }
          break;
        case 4:  // Duplicate a section
          if (pos + 5 <= input.size()) {
            std::string section = input.substr(pos, 5);
            input.insert(pos, section);
          }
          break;
        default:
          break;
      }
    }

    ASSERT_NO_FATAL_FAILURE(fuzzHttpRequestParsing(RawChars(input)));
  }
}

// Fuzz test with edge case patterns
TEST_F(HttpRequestViewTest, EdgeCasePatterns) {
  // Collection of edge case inputs
  static constexpr std::string_view kEdgeCases[]{
      "GET / HTTP/1.1\r\n",                                                   // No headers end
      "GET / HTTP/1.1\r\n\r\n",                                               // Minimal valid
      "GET / HTTP/1.1\r\nHost:\r\n\r\n",                                      // Empty header value
      "GET / HTTP/1.1\r\n: value\r\n\r\n",                                    // Empty header name
      "GET / HTTP/1.1\r\nKey\r\n\r\n",                                        // Missing colon
      "GET /%%%%%%%% HTTP/1.1\r\nHost: h\r\n\r\n",                            // Percent hell
      "GET / HTTP/1.1\r\nHost: h\r\nHost: h2\r\n\r\n",                        // Duplicate Host
      "GET / HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",     // Duplicate CL
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA / HTTP/1.1\r\nHost: h\r\n\r\n",  // Long method
  };

  for (std::string_view input : kEdgeCases) {
    ASSERT_NO_FATAL_FAILURE(fuzzHttpRequestParsing(RawChars(input)));
  }

  // Also fuzz with merge disabled
  for (std::string_view input : kEdgeCases) {
    ASSERT_NO_FATAL_FAILURE(fuzzHttpRequestParsing(RawChars(input), false));
  }
}

// Fuzz test with long headers/paths
TEST_F(HttpRequestViewTest, LongInputs) {
  // Long header name
  {
    std::string longName(1000, 'A');
    std::string reqStr = "GET / HTTP/1.1\r\n" + longName + ": v\r\n\r\n";
    RawChars buf;
    buf.append(reqStr);
    ASSERT_NO_FATAL_FAILURE(fuzzHttpRequestParsing(buf));
  }

  // Long header value
  {
    std::string longVal(10000, 'B');
    std::string reqStr = "GET / HTTP/1.1\r\nX: " + longVal + "\r\n\r\n";
    RawChars buf;
    buf.append(reqStr);
    ASSERT_NO_FATAL_FAILURE(fuzzHttpRequestParsing(buf));
  }

  // Long path
  {
    std::string longPath(5000, 'x');
    std::string reqStr = "GET /" + longPath + " HTTP/1.1\r\nHost: h\r\n\r\n";
    RawChars buf;
    buf.append(reqStr);
    ASSERT_NO_FATAL_FAILURE(fuzzHttpRequestParsing(buf));
  }
}

// Fuzz test specifically targeting header parsing
TEST_F(HttpRequestViewTest, HeaderParsingStress) {
  constexpr std::size_t kIterations = 100;

  RawChars input;
  for (std::size_t seed = 0; seed < kIterations; ++seed) {
    FuzzRng rng(seed + 3000000);

    input.clear();
    input.append("GET / HTTP/1.1\r\n");

    // Generate many headers
    std::size_t numHeaders = rng.range(0, 300);
    for (std::size_t headerPos = 0; headerPos < numHeaders; ++headerPos) {
      // Sometimes use known header names
      if (rng.coin()) {
        static constexpr std::string_view kKnownHeaders[]{
            http::Host,        http::ContentLength,    http::ContentType,
            "Accept",          "User-Agent",           "Authorization",
            "Cookie",          "Set-Cookie",           http::CacheControl,
            http::Connection,  http::TransferEncoding, http::AcceptEncoding,
            "Accept-Language",
        };
        input.append(kKnownHeaders[rng.range(0, std::size(kKnownHeaders))]);
      } else {
        std::size_t nameLen = rng.range(1, 30);
        for (std::size_t ii = 0; ii < nameLen; ++ii) {
          input.push_back(static_cast<char>('a' + rng.range(0, 26)));
        }
      }
      input.append(http::HeaderSep);

      // Value
      std::size_t valLen = rng.range(0, 1000);
      for (std::size_t ii = 0; ii < valLen; ++ii) {
        char ch = static_cast<char>(rng.range(32, 127));  // Printable ASCII
        input.push_back(ch);
      }
      input.append(http::CRLF);
    }
    input.append(http::CRLF);

    ASSERT_NO_FATAL_FAILURE(fuzzHttpRequestParsing(input));
  }
}

// ============================
// HttpRequestView::makeResponse tests
// ============================

TEST_F(HttpRequestViewTest, MakeResponseStatusCodeOnly) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse(http::StatusCodeAccepted);

  EXPECT_EQ(resp.status(), http::StatusCodeAccepted);
  EXPECT_TRUE(resp.bodyInMemory().empty());

  // Check that global headers are present
  EXPECT_TRUE(resp.hasHeader("server"));
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
}

TEST_F(HttpRequestViewTest, MakeResponseAdditionalCapacityStatusCodeOnly) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  static constexpr std::size_t kExtraCapacity = 64UL;
  auto resp = req.makeResponse(kExtraCapacity, http::StatusCodeAccepted);
  EXPECT_EQ(resp.status(), http::StatusCodeAccepted);
  EXPECT_TRUE(resp.bodyInMemory().empty());

  auto resp2 = req.makeResponse(http::StatusCodeAccepted);

  EXPECT_GE(resp.capacityInlined() + kExtraCapacity, resp2.capacityInlined());
  resp2.reserve(resp2.sizeInlined() + kExtraCapacity);
  EXPECT_EQ(resp.capacityInlined(), resp2.capacityInlined());

  // Check that global headers are present
  EXPECT_TRUE(resp.hasHeader("server"));
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
}

TEST_F(HttpRequestViewTest, HeaderAddLocationUnchecked) {
  ASSERT_EQ(reqSet(BuildRaw("GET", "/test", "HTTP/1.1")), http::StatusCodeOK);

  struct Case {
    std::string_view location;
    bool addSlash;
    std::string_view expected;
  };

  static constexpr std::string_view kBody = "Redirecting";

  // The location is a raw (percent-decoded) target: only its encoded form is written, so the header value is trimmed
  // and valid whatever the target.
  static constexpr Case kCases[] = {
      {"/foo", false, "/foo"},
      {"/foo", true, "/foo/"},
      {"", true, "/"},
      {"/a ", false, "/a%20"},
      {"/a ", true, "/a%20/"},
      {"/a\t", true, "/a%09/"},
      {" /a", false, "%20/a"},
      {"\t/a ", true, "%09/a%20/"},
      {"/a\r\nSet-Cookie: x=1", false, "/a%0D%0ASet-Cookie:%20x=1"},
      {"/100%/b?c#d", true, "/100%25/b%3Fc%23d/"},
      {"/caf\xC3\xA9", false, "/caf%C3%A9"},
      {"/a:b@c!$&'()*+,;=~._-", true, "/a:b@c!$&'()*+,;=~._-/"},
  };

  for (const auto& [location, addSlash, expected] : kCases) {
    SCOPED_TRACE(expected);

    const std::size_t locationHeaderSize = internal::HttpResponseAccess::LocationHeaderSize(location, addSlash);
    EXPECT_EQ(locationHeaderSize, http::HeaderSize(http::Location.size(), expected.size()));

    HttpResponse resp =
        req.makeResponse(HttpResponse::BodySize(kBody.size()) + locationHeaderSize, http::StatusCodeMovedPermanently);
    const std::size_t capacity = resp.capacityInlined();

    internal::HttpResponseAccess::HeaderAddLocationUnchecked(resp, location, addSlash);
    resp.body(kBody);

    // The reserved capacity was exactly the needed one: nothing reallocated.
    EXPECT_EQ(resp.capacityInlined(), capacity);
    EXPECT_EQ(resp.status(), http::StatusCodeMovedPermanently);
    EXPECT_TRUE(resp.hasHeader(http::Location));
    EXPECT_EQ(resp.headerValueOrEmpty(http::Location), expected);
    EXPECT_EQ(resp.bodyInMemory(), kBody);
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), http::ContentTypeTextPlain);
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), "11");
    EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  }
}

TEST_F(HttpRequestViewTest, HeaderAddLocationUncheckedLargeLocation) {
  ASSERT_EQ(reqSet(BuildRaw("GET", "/test", "HTTP/1.1")), http::StatusCodeOK);

  static constexpr std::size_t kNbChars = 64UL * 1024UL;
  static constexpr std::string_view kBody = "Redirecting";

  // One char out of three has to be encoded.
  std::string location(kNbChars, 'x');
  std::string expected;
  for (std::size_t pos = 0; pos < kNbChars; pos += 3UL) {
    location[pos] = ' ';
  }
  for (char ch : location) {
    if (ch == ' ') {
      expected.append("%20");
    } else {
      expected.push_back(ch);
    }
  }
  expected.push_back('/');

  const std::size_t locationHeaderSize = internal::HttpResponseAccess::LocationHeaderSize(location, true);
  EXPECT_EQ(locationHeaderSize, http::HeaderSize(http::Location.size(), expected.size()));

  HttpResponse resp =
      req.makeResponse(HttpResponse::BodySize(kBody.size()) + locationHeaderSize, http::StatusCodeMovedPermanently);
  const std::size_t capacity = resp.capacityInlined();

  internal::HttpResponseAccess::HeaderAddLocationUnchecked(resp, location, true);
  resp.body(kBody);

  EXPECT_EQ(resp.capacityInlined(), capacity);
  EXPECT_EQ(resp.headerValueOrEmpty(http::Location), expected);
  EXPECT_EQ(resp.bodyInMemory(), kBody);
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
}

TEST_F(HttpRequestViewTest, MakeResponseStatusCodeDefault200) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse();

  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_TRUE(resp.bodyInMemory().empty());
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
}

TEST_F(HttpRequestViewTest, MakeResponseBodyAndDefaultContentType) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse("Hello World");

  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.bodyInMemory(), "Hello World");
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), http::ContentTypeTextPlain);
}

TEST_F(HttpRequestViewTest, MakeResponseBodyAndCustomContentType) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse(R"({"key":"value"})", "application/json");

  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.bodyInMemory(), "{\"key\":\"value\"}");
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "application/json");
}

TEST_F(HttpRequestViewTest, MakeResponseStatusCodeBodyAndContentType) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse(http::StatusCodeCreated, "<html>OK</html>", "text/html");

  EXPECT_EQ(resp.status(), http::StatusCodeCreated);
  EXPECT_EQ(resp.bodyInMemory(), "<html>OK</html>");
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/html");
}

TEST_F(HttpRequestViewTest, MakeResponseBytesBodyAndDefaultContentType) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  const std::byte binaryData[]{
      std::byte{0x48}, std::byte{0x65}, std::byte{0x6c}, std::byte{0x6c}, std::byte{0x6f},
  };
  auto resp = req.makeResponse(std::span<const std::byte>{binaryData});

  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.bodyInMemory(), "Hello");
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), http::ContentTypeApplicationOctetStream);
}

TEST_F(HttpRequestViewTest, MakeResponseBytesBodyAndCustomContentType) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  const std::array<std::byte, 8> pngHeader = {
      std::byte{0x89}, std::byte{0x50}, std::byte{0x4e}, std::byte{0x47},
      std::byte{0x0d}, std::byte{0x0a}, std::byte{0x1a}, std::byte{0x0a},
  };
  auto resp = req.makeResponse(std::span<const std::byte>{pngHeader}, "image/png");

  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.bodyInMemory().size(), 8UL);
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "image/png");
}

TEST_F(HttpRequestViewTest, MakeResponseStatusCodeBytesBodyAndContentType) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  const std::array<std::byte, 4> data = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
  auto resp = req.makeResponse(http::StatusCodePartialContent, std::span<const std::byte>{data}, "application/binary");

  EXPECT_EQ(resp.status(), http::StatusCodePartialContent);
  EXPECT_EQ(resp.bodyInMemory().size(), 4UL);
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "application/binary");
}

TEST_F(HttpRequestViewTest, MakeResponseCanBeModifiedAfterCreation) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse(http::StatusCodeOK, "initial");
  resp.header("x-custom", "value");
  resp.header("x-another", "data");

  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  EXPECT_EQ(resp.headerValueOrEmpty("x-custom"), "value");
  EXPECT_EQ(resp.headerValueOrEmpty("x-another"), "data");
  EXPECT_EQ(resp.bodyInMemory(), "initial");
}

TEST_F(HttpRequestViewTest, MakeResponseEmptyBodyStillPrefillesGlobalHeaders) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse(http::StatusCodeNoContent);

  EXPECT_EQ(resp.status(), http::StatusCodeNoContent);
  EXPECT_TRUE(resp.bodyInMemory().empty());
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
}

TEST_F(HttpRequestViewTest, MakeResponseWithMultipleGlobalHeaders) {
  // Add multiple global headers
  globalHeaders.clear();
  globalHeaders.append("server: aeronet");
  globalHeaders.append("x-powered-by: aeronet");
  globalHeaders.append("x-version: 1.0");

  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse(http::StatusCodeOK, "test");

  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  EXPECT_EQ(resp.headerValueOrEmpty("x-powered-by"), "aeronet");
  EXPECT_EQ(resp.headerValueOrEmpty("x-version"), "1.0");
  EXPECT_EQ(resp.bodyInMemory(), "test");
}

TEST_F(HttpRequestViewTest, MakeResponseFromNullCStringHasNoBody) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  const char* nullBody = nullptr;
  auto resp = req.makeResponse(http::StatusCodeAccepted, nullBody);

  EXPECT_EQ(resp.status(), http::StatusCodeAccepted);
  EXPECT_FALSE(resp.hasBody());
  EXPECT_FALSE(resp.hasHeader(http::ContentType));
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
}

TEST_F(HttpRequestViewTest, MakeResponseCapturesOwnedBodiesWithoutCopy) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  static constexpr std::size_t kSize = 1024;

  // Checks that 'resp' captured the 'kSize' bytes at 'pData' and has the global headers.
  // 'resp' is taken by value (the callers' temporaries are elided into it): with a const reference, clang-tidy
  // wrongly reports performance-move-const-arg on the std::move() of the body inside the ternary arguments.
  const auto check = [](HttpResponse&& resp, http::StatusCode status, std::string_view contentType, const void* pData) {
    EXPECT_EQ(resp.status(), status);
    EXPECT_TRUE(resp.hasBodyCaptured());
    EXPECT_EQ(static_cast<const void*>(resp.bodyInMemory().data()), pData);
    EXPECT_EQ(resp.bodyLength(), kSize);
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), contentType);
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), "1024");
    EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  };

  for (http::StatusCode status : {http::StatusCodeOK, http::StatusCodeCreated}) {
    SCOPED_TRACE(status);
    const bool withStatus = status != http::StatusCodeOK;

    std::string str(kSize, 's');
    const void* pData = str.data();
    check(withStatus ? req.makeResponse(status, std::move(str)) : req.makeResponse(std::move(str)), status,
          http::ContentTypeTextPlain, pData);

    std::vector<char> vecChar(kSize, 'v');
    pData = vecChar.data();
    check(withStatus ? req.makeResponse(status, std::move(vecChar), "text/x-chars")
                     : req.makeResponse(std::move(vecChar), "text/x-chars"),
          status, "text/x-chars", pData);

    std::vector<std::byte> vecByte(kSize, std::byte{'b'});
    pData = vecByte.data();
    check(withStatus ? req.makeResponse(status, std::move(vecByte)) : req.makeResponse(std::move(vecByte)), status,
          http::ContentTypeApplicationOctetStream, pData);

    auto charBuf = std::make_unique<char[]>(kSize);
    pData = charBuf.get();
    check(
        withStatus ? req.makeResponse(status, std::move(charBuf), kSize) : req.makeResponse(std::move(charBuf), kSize),
        status, http::ContentTypeApplicationOctetStream, pData);

    auto bytesBuf = std::make_unique<std::byte[]>(kSize);
    pData = bytesBuf.get();
    check(withStatus ? req.makeResponse(status, std::move(bytesBuf), kSize, "image/png")
                     : req.makeResponse(std::move(bytesBuf), kSize, "image/png"),
          status, "image/png", pData);
  }
}

TEST_F(HttpRequestViewTest, MakeResponseCapturingEmptyBodyHasNoBody) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse(http::StatusCodeNoContent, std::string());

  EXPECT_EQ(resp.status(), http::StatusCodeNoContent);
  EXPECT_FALSE(resp.hasBody());
  EXPECT_FALSE(resp.hasHeader(http::ContentType));
  EXPECT_EQ(resp.headersFlatView(), req.makeResponse(http::StatusCodeNoContent).headersFlatView());
}

// The trailers of an HTTP/1.0 response are dropped (it cannot be chunked): a HEAD response does not announce them.
TEST_F(HttpRequestViewTest, MakeResponseForHttp10HeadRequestAnnouncesNoTrailer) {
  setAddTrailerHeader(true);
  for (std::string_view version : {"HTTP/1.0", "HTTP/1.1"}) {
    ASSERT_EQ(reqSet(BuildRaw("HEAD", "/test", version)), http::StatusCodeOK);
    HttpResponse resp = req.makeResponse("body");
    resp.trailerAddLine("x-checksum", "abc");
    EXPECT_EQ(resp.headerValueOrEmpty(http::Trailer), version == "HTTP/1.1" ? "x-checksum" : "") << version;
  }
}

TEST_F(HttpRequestViewTest, MakeResponseCapturedBodyForHeadRequestKeepsOnlyItsSize) {
  auto st = reqSet(BuildRaw("HEAD", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse(std::string(100, 'h'), "text/html");
  auto expected = req.makeResponse();
  expected.body(std::string(100, 'h'), "text/html");

  EXPECT_EQ(resp.headersFlatView(), expected.headersFlatView());
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), "100");
  EXPECT_EQ(resp.bodyLength(), 100U);
  EXPECT_EQ(resp.bodyLength(), expected.bodyLength());
  EXPECT_EQ(resp.bodyInMemory(), expected.bodyInMemory());
}

#ifdef AERONET_ENABLE_GLAZE
// Must have external linkage for Glaze's compile-time reflection (no anonymous namespace).
// NOLINTNEXTLINE(misc-use-internal-linkage)
struct JsonPoint {
  int x{};
  int y{};
};

TEST_F(HttpRequestViewTest, BodyAsJsonParsesValidPayload) {
  auto st = reqSet(BuildRaw("POST", "/json", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);
  setAggregatedBody(R"({"x":3,"y":7})");

  auto result = req.bodyAs<JsonPoint>();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->x, 3);
  EXPECT_EQ(result->y, 7);
}

TEST_F(HttpRequestViewTest, BodyAsJsonReturnsBadRequestOnParseError) {
  auto st = reqSet(BuildRaw("POST", "/json", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);
  setAggregatedBody("not json at all");

  auto result = req.bodyAs<JsonPoint>();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().status(), http::StatusCodeBadRequest);
}

TEST_F(HttpRequestViewTest, BodyAsYamlParsesValidPayload) {
  auto st = reqSet(BuildRaw("POST", "/yaml", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);
  setAggregatedBody("x: 5\ny: 9\n");

  auto result = req.bodyAsYaml<JsonPoint>();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->x, 5);
  EXPECT_EQ(result->y, 9);
}

TEST_F(HttpRequestViewTest, BodyAsYamlReturnsBadRequestOnParseError) {
  auto st = reqSet(BuildRaw("POST", "/yaml", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);
  setAggregatedBody("x: : : not yaml\n\t- broken");

  auto result = req.bodyAsYaml<JsonPoint>();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().status(), http::StatusCodeBadRequest);
}
#endif  // AERONET_ENABLE_GLAZE

}  // namespace aeronet
