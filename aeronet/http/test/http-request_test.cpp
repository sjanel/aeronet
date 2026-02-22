#include "aeronet/http-request.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response-prefinalize.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/unix-dogstatsd-sink.hpp"

namespace aeronet {

namespace {
// Helper to build a raw HTTP request buffer we can feed into HttpRequest::setHead
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

}  // namespace

class HttpRequestTest : public ::testing::Test {
 protected:
  void SetUp() override {
    globalHeaders.append("server: aeronet");
    req._ownerState = &cs;
    req._pGlobalHeaders = &globalHeaders;
    req._pCompressionState = &compressionState;

    compressionState.pCompressionConfig = &compressionConfig;
  }

  http::StatusCode reqSet(RawChars raw, bool mergeAllowedForUnknownRequestHeaders = true,
                          std::size_t maxHeaderSize = 4096UL) {
    cs.inBuffer = std::move(raw);
    RawChars tmpBuffer;
    auto ret = req.initTrySetHead(cs.inBuffer, tmpBuffer, maxHeaderSize, mergeAllowedForUnknownRequestHeaders, nullptr);
    if (ret == http::StatusCodeOK) {
      req.finalizeBeforeHandlerCall({});
    }
    return ret;
  }

  http::StatusCode reqSetWithSpan(RawChars raw, tracing::SpanPtr span, bool mergeAllowedForUnknownRequestHeaders = true,
                                  std::size_t maxHeaderSize = 4096UL) {
    cs.inBuffer = std::move(raw);
    RawChars tmpBuffer;
    auto ret = req.initTrySetHead(cs.inBuffer, tmpBuffer, maxHeaderSize, mergeAllowedForUnknownRequestHeaders,
                                  std::move(span));
    if (ret == http::StatusCodeOK) {
      req.finalizeBeforeHandlerCall({});
    }
    return ret;
  }

  void checkHeaders(std::initializer_list<http::HeaderView> headers) {
    for (const auto& [key, val] : headers) {
      EXPECT_EQ(req.headerValueOrEmpty(key), val);
    }
  }

  void rehash(std::size_t capacity) { req._headers.rehash(capacity); }

  void shrinkAndMaybeClear() { req.shrinkAndMaybeClear(); }

  // Helpers that exercise private internals via friendship with HttpRequest.
  void setBodyAccessAggregated() { req._bodyAccessMode = HttpRequest::BodyAccessMode::Aggregated; }
  void setBodyAccessStreamingWithBridgeNoHasMore() {
    req._bodyAccessMode = HttpRequest::BodyAccessMode::Streaming;
    static HttpRequest::BodyAccessBridge bridge;
    bridge.readChunk = [](HttpRequest&, void*, std::size_t) -> std::string_view { return {}; };
    bridge.hasMore = [](const HttpRequest&, void*) -> bool { return false; };
    req._bodyAccessBridge = &bridge;
  }

  void setResponsePossibleEncoding(Encoding encoding) { req._responsePossibleEncoding = encoding; }

  void setCompressionState(internal::ResponseCompressionState* state) { req._pCompressionState = state; }

  // Test helpers that require friend access to HttpRequest private members
  struct FakeSpan : public tracing::Span {
    static inline int64_t lastStatusCode = -1;
    static inline long lastDurationUs = -1;
    static inline bool ended = false;
    static inline bool sawHttpHost = false;

    void setAttribute(std::string_view key, int64_t val) noexcept override {
      if (key == "http.status_code") {
        lastStatusCode = val;
      } else if (key == "http.duration_us") {
        lastDurationUs = static_cast<long>(val);
      }
    }

    void setAttribute(std::string_view key, [[maybe_unused]] std::string_view val) noexcept override {
      if (key == "http.host") {
        sawHttpHost = true;
      }
    }

    void end() noexcept override { ended = true; }
  };

  static std::string_view bridgeReadChunk(HttpRequest& /*req*/, void* /*ctx*/, std::size_t /*maxBytes*/) {
    return {"chunk-data"};
  }

  static bool bridgeHasMore(const HttpRequest& /*req*/, void* /*ctx*/) { return true; }

  static std::string_view bridgeAggregate(HttpRequest& /*req*/, void* /*ctx*/) { return {"full-body"}; }

  // Helpers that perform operations requiring friendship (call private members on req)
  void installStreamingBridge() {
    static HttpRequest::BodyAccessBridge bridge;
    bridge.readChunk = bridgeReadChunk;
    bridge.hasMore = bridgeHasMore;
    req._bodyAccessBridge = &bridge;
  }

  void installAggregateBridge() {
    static HttpRequest::BodyAccessBridge bridge;
    bridge.aggregate = bridgeAggregate;
    req._bodyAccessBridge = &bridge;
  }

  // Helper to set a custom BodyAccessBridge and explicitly clear the context.
  void setCustomBridgeWithNullContext(HttpRequest::BodyAccessBridge::AggregateFn aggregate,
                                      HttpRequest::BodyAccessBridge::ReadChunkFn readChunk,
                                      HttpRequest::BodyAccessBridge::HasMoreFn hasMore) {
    static HttpRequest::BodyAccessBridge sbridge;
    sbridge.aggregate = aggregate;
    sbridge.readChunk = readChunk;
    sbridge.hasMore = hasMore;
    req._bodyAccessBridge = &sbridge;
    req._bodyAccessContext = nullptr;
  }

  // Fixture-level helper to mutate the request's private body access context
  void setRequestBodyAccessContextToNull() { cs.request._bodyAccessContext = nullptr; }

  void setOwnerState(ConnectionState* st) { req._ownerState = st; }

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  void callPinHeadStorage() { req.pinHeadStorage(cs); }
#endif

  // Helper to set a header view pointing at arbitrary bytes (fixture has friend access)
  void setHeaderViewToPtr(std::string_view key, const char* dataPtr, std::size_t len) {
    req._headers.try_emplace(key, dataPtr, len);
  }
  void setTrailerViewToPtr(std::string_view key, const char* dataPtr, std::size_t len) {
    req._trailers.try_emplace(key, dataPtr, len);
  }

  void setPathParamToPtr(std::string_view key, const char* dataPtr, std::size_t len) {
    req._pathParams.try_emplace(key, dataPtr, len);
  }

  [[nodiscard]] bool callWantClose() const { return req.wantClose(); }

  void attachSpan(tracing::SpanPtr span) { req._traceSpan = std::move(span); }

  void callEnd(http::StatusCode sc) { req.end(sc); }

  // Feed raw bytes to HttpRequest parser and ensure no crash/UB
  void fuzzHttpRequestParsing(const RawChars& input, bool mergeUnknownHeaders = true) {
    // Parse with various max header sizes
    for (std::size_t maxSize : {64UL, 256UL, 1024UL, 8192UL}) {
      cs.inBuffer = input;  // reset
      [[maybe_unused]] auto status = reqSet(input, mergeUnknownHeaders, maxSize);
      // We don't care about the result - just that we don't crash
      (void)status;
    }
  }

  ConcatenatedHeaders globalHeaders;
  CompressionConfig compressionConfig;
  internal::ResponseCompressionState compressionState;
  HttpRequest req;
  ConnectionState cs;
};

TEST_F(HttpRequestTest, ReadBodyWithZeroMaxBytesReturnsEmpty) {
  setBodyAccessStreamingWithBridgeNoHasMore();
  auto chunk = req.readBody(0);
  EXPECT_TRUE(chunk.empty());
}

TEST_F(HttpRequestTest, PrefinalizeCompressionExceedsMaxRatioIncrementsMetric) {
  for (Encoding encoding : test::SupportedEncodings()) {
    test::UnixDogstatsdSink sink;
    TelemetryConfig tcfg;
    tcfg.withDogStatsdSocketPath(sink.path()).withDogStatsdNamespace("svc").enableDogStatsDMetrics(true);
    tracing::TelemetryContext telemetryContext(tcfg);

    compressionConfig.minBytes = 1U;
    compressionConfig.maxCompressRatio = 0.01F;
    compressionState = internal::ResponseCompressionState(compressionConfig);
    setCompressionState(&compressionState);
    setResponsePossibleEncoding(encoding);

    HttpResponse resp(http::StatusCodeOK);
    auto body = test::MakeRandomPayload(2 << 10);
    resp.body(body, http::ContentTypeTextPlain);

    internal::PrefinalizeHttpResponse(req, resp, false, compressionState, telemetryContext);

    EXPECT_EQ(sink.recvMessage(), "svc.aeronet.http_responses.compression.exceeds_max_ratio_total:1|c");
  }
}

TEST_F(HttpRequestTest, BridgeWithNullContextAggregateHandledGracefully) {
  // Aggregate accessor: null context -> empty body
  using AggFnRaw = std::string_view (*)(HttpRequest&, void*);
  AggFnRaw agg = +[](HttpRequest& /*r*/, void* ctx) -> std::string_view {
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

TEST_F(HttpRequestTest, BridgeWithNullContextStreamingHandledGracefully) {
  // Streaming accessor: null context -> empty chunks and hasMore false
  using ReadFnRaw = std::string_view (*)(HttpRequest&, void*, std::size_t);
  using HasMoreFnRaw = bool (*)(const HttpRequest&, void*);

  ReadFnRaw rd = +[](HttpRequest& /*r*/, void* ctx, std::size_t /*maxBytes*/) -> std::string_view {
    if (ctx == nullptr) {
      return {};
    }
    return "c";
  };
  HasMoreFnRaw hm = +[](const HttpRequest& /*r*/, void* ctx) -> bool { return ctx != nullptr; };

  setCustomBridgeWithNullContext(nullptr, rd, hm);

  // readBody should return empty for null context (streaming)
  EXPECT_TRUE(req.readBody(4).empty());
  EXPECT_FALSE(req.hasMoreBody());
}

TEST_F(HttpRequestTest, AggregatedBridgeNullContextAndHasMoreHandled) {
  // Install the real aggregated bridge via ConnectionState so the bridge points
  // at ConnectionState::bodyStreamContext functions defined in connection-state.cpp.
  cs.installAggregatedBodyBridge();

  // Force the bridge context to be null to exercise the null-context branches
  // inside AggregateBufferedBody and HasMoreBufferedBody.
  setRequestBodyAccessContextToNull();

  // Aggregate accessor with null context should return empty and not crash.
  EXPECT_TRUE(cs.request.body().empty());

  // hasMore should return false when context is null.
  EXPECT_FALSE(cs.request.hasMoreBody());
}

TEST_F(HttpRequestTest, BridgePresentButAggregateNull) {
  // Removed: test constructed a BodyAccessBridge directly which uses private
  // types; use BridgePointerPresentButAggregateNull which installs the bridge
  // via the provided helper instead.
}

TEST_F(HttpRequestTest, BridgePointerPresentButAggregateNull) {
  // Install a bridge where the bridge pointer is non-null but aggregate is
  // explicitly null. Use the helper that sets the bridge and nulls the
  // context. This should exercise the `_bodyAccessBridge != nullptr` true
  // path while `aggregate == nullptr` is false, so body() must not call
  // aggregate and should return an empty view.
  setCustomBridgeWithNullContext(/*aggregate=*/nullptr, /*readChunk=*/nullptr, /*hasMore=*/nullptr);

  EXPECT_TRUE(req.body().empty());
  EXPECT_FALSE(req.hasMoreBody());
}

TEST_F(HttpRequestTest, AggregatedBridgeReadOffsetPastEndHandled) {
  // Install aggregated bridge and ensure the body context is present.
  cs.installAggregatedBodyBridge();

  // Provide an empty body so offset (0) is already past/equal to size (0).
  cs.bodyStreamContext.body = std::string_view();
  cs.bodyStreamContext.offset = 0;

  // readBody should see offset >= body.size() and return empty without crashing.
  auto chunk = cs.request.readBody(4);
  EXPECT_TRUE(chunk.empty());
}

TEST_F(HttpRequestTest, AggregatedBridgeHasMoreNullContextHandled) {
  // Install aggregated bridge via ConnectionState
  cs.installAggregatedBodyBridge();

  // Ensure context is null to hit the null-context branch in HasMoreBufferedBody
  setRequestBodyAccessContextToNull();

  // hasMoreBody should return false when context is null
  EXPECT_FALSE(cs.request.hasMoreBody());
}

TEST_F(HttpRequestTest, TraceSpanNotSetWhenNoHostHeader) {
  // Build a raw request without a Host header and pass a FakeSpan into
  // initTrySetHead; the span should receive http.method, http.target, and
  // http.scheme but not http.host.
  RawChars raw;
  raw.append("GET /nohost HTTP/1.1\r\n");
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

TEST_F(HttpRequestTest, PinHead_NoHeadSpan_Noop) {
  // When no head span is present, pinHeadStorage should be a no-op.
  EXPECT_EQ(req.headSpanSize(), 0);

  callPinHeadStorage();

  EXPECT_EQ(req.headSpanSize(), 0);
}

TEST_F(HttpRequestTest, PinHead_NormalCopiesAndRemaps) {
  // Build a simple request and initialize head so _headSpanSize is set
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1", "X-Test: v\r\n");
  auto st = reqSet(std::move(raw));
  ASSERT_EQ(st, http::StatusCodeOK);

  // Before pinning, header views point into cs.inBuffer
  auto before = req.headerValueOrEmpty("X-Test");
  EXPECT_EQ(before, "v");

  // Call pinHeadStorage to copy head into headBuffer and remap views
  callPinHeadStorage();
  EXPECT_GT(req.headSpanSize(), 0);

  // Header value should still be accessible and unchanged after pin
  EXPECT_EQ(req.headerValueOrEmpty("X-Test"), "v");
}

TEST_F(HttpRequestTest, PinHead_SecondCallIsNoop) {
  // Build and pin once
  auto raw = BuildRaw("GET", "/p2", "HTTP/1.1", "X-A: b\r\n");
  auto st = reqSet(std::move(raw));
  ASSERT_EQ(st, http::StatusCodeOK);
  callPinHeadStorage();
  EXPECT_GT(req.headSpanSize(), 0);

  // Capture header pointer after first pin
  auto val1 = req.headerValueOrEmpty("X-A");

  // Second call should be a no-op and not crash/change values
  callPinHeadStorage();
  EXPECT_GT(req.headSpanSize(), 0);
  auto val2 = req.headerValueOrEmpty("X-A");
  EXPECT_EQ(val1, val2);
}

TEST_F(HttpRequestTest, HasMoreBodyNeedsBothActiveAndNeedsBody) {
  // Ensure _ownerState is set so hasMoreBody() consults asyncState
  setOwnerState(&cs);

  cs.asyncState.active = false;
  cs.asyncState.needsBody = true;

  EXPECT_FALSE(req.hasMoreBody());

  cs.asyncState.active = true;
  cs.asyncState.needsBody = false;
  EXPECT_FALSE(req.hasMoreBody());

  cs.asyncState.needsBody = true;
  EXPECT_TRUE(req.hasMoreBody());
}

#endif

TEST_F(HttpRequestTest, NotEnoughDataNoEndOfHeaders) {
  EXPECT_EQ(reqSet(BuildRaw("GET", "/", "HTTP/1.1", "Server: aeronet", false)), 0);
}

TEST_F(HttpRequestTest, InvalidHttpVersion) {
  EXPECT_EQ(reqSet(BuildRaw("GET", "/", "HTTP")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /path HTTP1.1\r\n\r\n")), http::StatusCodeBadRequest);
}

TEST_F(HttpRequestTest, InvalidHeaderKey) {
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\n:value\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\n  :value\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\nHeaderKey :value\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\n\tHeaderKey:value\r\n")), http::StatusCodeBadRequest);
}

TEST_F(HttpRequestTest, InvalidHeaderKeyValueSeparator) {
  EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\nKey;Value\r\n")), http::StatusCodeBadRequest);
}

TEST_F(HttpRequestTest, NoCRLF) { EXPECT_EQ(reqSet(RawChars("GET")), 0); }

TEST_F(HttpRequestTest, InvalidMethod) {
  EXPECT_EQ(reqSet(RawChars("GETA / HTTP/1.1\r\n\r\n")), http::StatusCodeNotImplemented);
}

TEST_F(HttpRequestTest, InvalidPath) {
  EXPECT_EQ(reqSet(RawChars("GET   HTTP/1.1\r\n\r\n")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(RawChars("GET ?a=b HTTP/1.1\r\n\r\n")), http::StatusCodeBadRequest);
}

TEST_F(HttpRequestTest, NotEnoughDataOnlyFirstLine) { EXPECT_EQ(reqSet(RawChars("GET /test HTTP/1.0\r\n")), 0); }

TEST_F(HttpRequestTest, ParseBasicPathAndVersion) {
  auto st = reqSet(BuildRaw("GET", "/abc", "HTTP/1.1"));
  EXPECT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.method(), http::Method::GET);
  EXPECT_EQ(req.path(), "/abc");
  EXPECT_EQ(req.version(), http::HTTP_1_1);
  EXPECT_TRUE(req.queryParams().begin() == req.queryParams().end());
}

TEST_F(HttpRequestTest, QueryParamsDecodingPlusAndPercent) {
  // a=1+2&b=hello%20world&c=%zz (malformed % sequence left verbatim for c's value)
  auto st = reqSet(BuildRaw("GET", "/p?a=1+2&b=hello%20world&c=%zz"));
  ASSERT_EQ(st, http::StatusCodeOK);
  std::vector<http::HeaderView> seen;
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

TEST_F(HttpRequestTest, QueryParamInt) {
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

TEST_F(HttpRequestTest, EmptyAndMissingValues) {
  auto st = reqSet(BuildRaw("GET", "/p?k1=&k2&=v"));
  ASSERT_EQ(st, http::StatusCodeOK);
  std::vector<http::HeaderView> seen;
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

TEST_F(HttpRequestTest, QueryParamsRangeDuplicateKeysPreservedOrder) {
  auto st = reqSet(BuildRaw("GET", "/p?x=1&x=2&x=3"));
  ASSERT_EQ(st, http::StatusCodeOK);
  std::vector<std::string_view> values;
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

TEST_F(HttpRequestTest, InvalidPathEscapeCauses400) {
  auto st = reqSet(BuildRaw("GET", "/bad%zz"));
  EXPECT_EQ(st, http::StatusCodeBadRequest);
}

TEST_F(HttpRequestTest, HeaderAccessorsBasicAndEmptyVsMissing) {
  // Provide headers including:
  //  - normal value (X-Test)
  //  - empty value (X-Empty)
  //  - value with trailing spaces (X-Trim)
  //  - value with leading & trailing mixed whitespace (X-Spaces)
  //  - lowercase key to verify case-insensitive lookup (content-length)
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "X-Empty:\r\n"
                            "X-Trim: value   \r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  // Existing normal header
  EXPECT_EQ(req.headerValueOrEmpty("X-Test"), "Value");
  EXPECT_EQ(req.headerValue("X-Test").value_or(""), "Value");

  // Case-insensitive lookup
  EXPECT_EQ(req.headerValueOrEmpty("x-test"), "Value");
  EXPECT_TRUE(req.headerValue("x-test").has_value());

  // Empty header value vs missing header
  EXPECT_EQ(req.headerValueOrEmpty("X-Empty"), "");
  EXPECT_TRUE(req.headerValue("X-Empty").has_value());

  // Trimming behavior (trailing)
  EXPECT_EQ(req.headerValueOrEmpty("X-Trim"), "value");
  EXPECT_EQ(req.headerValueOrEmpty("x-trim"), "value");
  // Trimming behavior (leading & trailing)
  EXPECT_EQ(req.headerValueOrEmpty("X-Spaces"), "abc");
  EXPECT_EQ(req.headerValue("X-Spaces").value_or(""), "abc");

  EXPECT_EQ(req.headerValueOrEmpty("No-Such"), std::string_view());
  EXPECT_FALSE(req.headerValue("No-Such").has_value());
}

TEST_F(HttpRequestTest, HeaderAccessorsAbsentHeaders) {
  auto st = reqSet(BuildRaw("GET", "/p"));
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("Host"), "h");  // baseline sanity
  EXPECT_EQ(req.headerValueOrEmpty("X-Unknown"), std::string_view());
  EXPECT_FALSE(req.headerValue("X-Unknown").has_value());
}

TEST_F(HttpRequestTest, MergeConsecutiveHeaders) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H:v1\r\n"
                            "H:v2\r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v1,v2"}, {"X-Spaces", "abc"}, {http::ContentLength, "0"}});
}

TEST_F(HttpRequestTest, ShrinkToFit) {
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

TEST_F(HttpRequestTest, MergeConsecutiveHeadersWithSpaces) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H: v1  \r\n"
                            "H: v2\r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v1,v2"}, {"X-Spaces", "abc"}, {http::ContentLength, "0"}});
}

TEST_F(HttpRequestTest, MergeNonConsecutiveHeaders) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H:v1\r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "H:v2\r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v1,v2"}, {"X-Spaces", "abc"}, {http::ContentLength, "0"}});
}

TEST_F(HttpRequestTest, MergeNonConsecutiveHeadersWithSpaces) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H: v1  \r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "H: v2\r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v1,v2"}, {"X-Spaces", "abc"}, {http::ContentLength, "0"}});
}

TEST_F(HttpRequestTest, MergeNonConsecutiveHeadersWithEmptyOnFirst) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H:  \r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "H:v2\r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v2"}, {"X-Spaces", "abc"}, {http::ContentLength, "0"}});
}

TEST_F(HttpRequestTest, MergeNonConsecutiveHeadersWithEmptyOnSecond) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H: v1  \r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "H:\r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v1"}, {"X-Spaces", "abc"}, {http::ContentLength, "0"}});
}

TEST_F(HttpRequestTest, MergeNonConsecutiveHeadersBothEmpty) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H:   \r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "H:\r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", ""}, {"X-Spaces", "abc"}, {http::ContentLength, "0"}});

  EXPECT_TRUE(req.headerValue("H").has_value());
}

TEST_F(HttpRequestTest, MergeMultipleCookies) {
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

  checkHeaders({{"X-Test", "Value"},
                {"Cookie", "cookie1;cookie2;cookie3;cookie4"},
                {"X-Spaces", "abc"},
                {http::ContentLength, "0"}});
}

TEST_F(HttpRequestTest, MergeMultipleHeaders) {
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

  checkHeaders({{"X-Test", "Value"},
                {"Cookie", "cookie1;cookie2;cookie3;cookie4"},
                {"X-Spaces", "abc,de,fgh"},
                {http::ContentLength, "0"}});

  // merge not allowed for custom header X-Spaces
  st = reqSet(std::move(raw), false);
  ASSERT_EQ(st, http::StatusCodeBadRequest);
}

TEST_F(HttpRequestTest, AcceptHeaderCommaMerge) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "Accept: text/plain\r\n"
                            "Accept: text/html\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("Accept"), "text/plain,text/html");
}

TEST_F(HttpRequestTest, AcceptHeaderSkipEmptySecond) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "Accept: text/plain\r\n"
                            "Accept:   \r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("Accept"), "text/plain");
}

TEST_F(HttpRequestTest, AcceptHeaderEmptyFirstTakesSecond) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "Accept:    \r\n"
                            "Accept: text/html\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("Accept"), "text/html");
}

TEST_F(HttpRequestTest, UserAgentSpaceMerge) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "User-Agent: Foo  \r\n"
                            "User-Agent:   Bar   \r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("User-Agent"), "Foo Bar");
}

TEST_F(HttpRequestTest, AuthorizationOverrideKeepsLast) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "Authorization: Bearer first\r\n"
                            "Authorization: Bearer second\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("Authorization"), "Bearer second");
}

TEST_F(HttpRequestTest, AuthorizationEmptyFirstThenValue) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "Authorization:   \r\n"
                            "Authorization: Bearer token\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("Authorization"), "Bearer token");
}

TEST_F(HttpRequestTest, AuthorizationOverrideCaseInsensitive) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "aUtHoRiZaTiOn: Bearer First\r\n"
                            "AUTHORIZATION: Bearer Second\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("Authorization"), "Bearer Second");
}

TEST_F(HttpRequestTest, HasMoreBodyReturnsFalseWhenAggregated) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  setBodyAccessAggregated();
  EXPECT_FALSE(req.hasMoreBody());
}

TEST_F(HttpRequestTest, ReadBufferedBodyNullContextReturnsEmpty) {
  // Install aggregated bridge via ConnectionState helper
  cs.installAggregatedBodyBridge();

  // Force the bridge context to be null to exercise the null-context branch
  setRequestBodyAccessContextToNull();

  // Calling readBody should return empty when context is null
  auto chunk = cs.request.readBody(4);
  EXPECT_TRUE(chunk.empty());
}

TEST_F(HttpRequestTest, HasMoreBodyReturnsFalseWhenBridgeHasNoHasMore) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  setBodyAccessStreamingWithBridgeNoHasMore();
  EXPECT_FALSE(req.hasMoreBody());
}

TEST_F(HttpRequestTest, BodyAfterReadBodyThrows) {
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

TEST_F(HttpRequestTest, ReadBodyAfterBodyThrows) {
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

TEST_F(HttpRequestTest, HasMoreBodyShouldBeFalseByDefault) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  EXPECT_FALSE(req.hasMoreBody());
}

TEST_F(HttpRequestTest, Http2FieldsShouldBeFilledCorrectlyInHttp1) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  EXPECT_FALSE(req.isHttp2());
  EXPECT_EQ(req.streamId(), 0);
  EXPECT_TRUE(req.scheme().empty());
  EXPECT_TRUE(req.authority().empty());
}

TEST_F(HttpRequestTest, ReadBodyWithBridgeReturnsChunk) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  installStreamingBridge();

  auto chunk = req.readBody(64);
  EXPECT_EQ(chunk, "chunk-data");

  EXPECT_TRUE(req.isBodyReady());
}

TEST_F(HttpRequestTest, HasMoreBodyWithBridgeTrue) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  installStreamingBridge();

  EXPECT_TRUE(req.hasMoreBody());
}

TEST_F(HttpRequestTest, BodyWithAggregateBridgeReturnsFullBody) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  installAggregateBridge();

  auto full = req.body();
  EXPECT_EQ(full, "full-body");

  EXPECT_TRUE(req.isBodyReady());
}

TEST_F(HttpRequestTest, BodyShouldBeReadyIfBodyCalled) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  EXPECT_FALSE(req.isBodyReady());
  [[maybe_unused]] auto full = req.body();
  EXPECT_TRUE(req.isBodyReady());
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS

TEST_F(HttpRequestTest, PinHeadStorageRemapsViews) {
  // Build raw request with a header value we can inspect pointer for
  RawChars raw = BuildRaw("GET", "/p", "HTTP/1.1", "X-Custom: original_value\r\n");
  auto st = reqSet(std::move(raw));
  ASSERT_EQ(st, http::StatusCodeOK);

  // capture original view pointer into connection inBuffer
  auto hostView = req.headerValueOrEmpty("X-Custom");
  const char* originalPtr = hostView.data();

  // now pin head storage which moves data into state.asyncState.headBuffer
  callPinHeadStorage();

  // after pinning, header view should point into headBuffer (ownerState.headBuffer)
  auto pinned = req.headerValueOrEmpty("X-Custom");
  const char* pinnedPtr = pinned.data();

  ASSERT_NE(originalPtr, pinnedPtr);
  // pinned pointer should be within headBuffer data range
  const char* hb = cs.asyncState.headBuffer.data();
  EXPECT_GE(pinnedPtr, hb);
  EXPECT_LT(pinnedPtr, hb + static_cast<std::ptrdiff_t>(cs.asyncState.headBuffer.size()));
}

TEST_F(HttpRequestTest, PinHead_SkipsRemapForViewsBeyondOldLimit) {
  // Build a simple request to populate head span
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1", "X-A: a\r\n");
  auto st = reqSet(std::move(raw));
  ASSERT_EQ(st, http::StatusCodeOK);

  // Ensure there is extra data in the connection buffer after the head span
  const char* extra = "EXTRA_PAYLOAD_DATA";
  cs.inBuffer.append(extra, static_cast<std::size_t>(std::strlen(extra)));

  // Create a header whose value points to bytes beyond the head span (>= oldLimit)
  const char* oldBase = cs.inBuffer.data();
  const char* beyond = oldBase + static_cast<std::ptrdiff_t>(req.headSpanSize()) + 2;
  // Use fixture helper to add header view pointing into connection buffer beyond oldLimit
  setHeaderViewToPtr("X-Outside", beyond, 5);

  // Record pointer before pinning
  auto before = req.headerValueOrEmpty("X-Outside");
  const char* beforePtr = before.data();

  // Call pin which should NOT remap the view (it lies beyond oldLimit)
  callPinHeadStorage();

  auto after = req.headerValueOrEmpty("X-Outside");
  const char* afterPtr = after.data();

  EXPECT_EQ(beforePtr, afterPtr);

  // Ensure the after pointer still points into connection inBuffer (not into headBuffer)
  const char* inBase = cs.inBuffer.data();
  EXPECT_GE(afterPtr, inBase);
  EXPECT_LT(afterPtr, inBase + static_cast<std::ptrdiff_t>(cs.inBuffer.size()));
  // And ensure it's not inside headBuffer
  const char* hb = cs.asyncState.headBuffer.data();
  EXPECT_FALSE(afterPtr >= hb && afterPtr < hb + static_cast<std::ptrdiff_t>(cs.asyncState.headBuffer.size()));
}

TEST_F(HttpRequestTest, PinHead_SkipsRemapForViewsBeforeOldBase) {
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
  setHeaderViewToPtr("X-Before", before, 3);

  auto beforeView = req.headerValueOrEmpty("X-Before");
  const char* beforePtr = beforeView.data();

  callPinHeadStorage();

  auto afterView = req.headerValueOrEmpty("X-Before");
  const char* afterPtr = afterView.data();

  // Pointer should be unchanged (remap skipped)
  EXPECT_EQ(beforePtr, afterPtr);

  // Ensure it still points into tmp buffer (not into headBuffer)
  const char* hb = cs.asyncState.headBuffer.data();
  EXPECT_FALSE(afterPtr >= hb && afterPtr < hb + static_cast<std::ptrdiff_t>(cs.asyncState.headBuffer.size()));
}

TEST_F(HttpRequestTest, PinHead_RemapsEntriesInsideOldSpan) {
  // Build request with headers so head span contains header values we can reference
  RawChars raw = BuildRaw("GET", "/p", "HTTP/1.1", "X-Remap: val\r\n");
  auto st = reqSet(std::move(raw));
  ASSERT_EQ(st, http::StatusCodeOK);

  // Capture pointer into the current inBuffer for header value
  auto hv = req.headerValueOrEmpty("X-Remap");
  const char* origPtr = hv.data();

  // Also set a trailer and a path param value pointing inside the head span by using the same pointer
  setTrailerViewToPtr("T-Remap", origPtr, 3);
  setPathParamToPtr("pp", origPtr, 3);

  // Pin head storage which should remap entries that are inside the old span
  callPinHeadStorage();

  // After pinning, header value pointer must have changed (moved into headBuffer)
  auto pinnedHeader = req.headerValueOrEmpty("X-Remap");
  const char* pinnedPtr = pinnedHeader.data();
  EXPECT_NE(origPtr, pinnedPtr);

  // Trailer and path param entries should also have been remapped
  auto tr = req.trailers().find("T-Remap");
  ASSERT_TRUE(tr != req.trailers().end());
  const char* trPtr = tr->second.data();
  EXPECT_NE(origPtr, trPtr);

  auto ppIt = req.pathParams().find("pp");
  ASSERT_TRUE(ppIt != req.pathParams().end());
  const char* ppPtr = ppIt->second.data();
  EXPECT_NE(origPtr, ppPtr);

  // All remapped pointers should be inside headBuffer
  const char* hb = cs.asyncState.headBuffer.data();
  EXPECT_GE(pinnedPtr, hb);
  EXPECT_LT(pinnedPtr, hb + static_cast<std::ptrdiff_t>(cs.asyncState.headBuffer.size()));
  EXPECT_GE(trPtr, hb);
  EXPECT_LT(trPtr, hb + static_cast<std::ptrdiff_t>(cs.asyncState.headBuffer.size()));
  EXPECT_GE(ppPtr, hb);
  EXPECT_LT(ppPtr, hb + static_cast<std::ptrdiff_t>(cs.asyncState.headBuffer.size()));
}

#endif

TEST_F(HttpRequestTest, WantCloseAndHasExpectContinue) {
  {  // Connection: close
    auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1", MakeHttp1HeaderLine(http::Connection, "close")));
    ASSERT_EQ(st, http::StatusCodeOK);
    EXPECT_TRUE(callWantClose());
    EXPECT_FALSE(req.hasExpectContinue());
  }
  {  // Expect: 100-continue on HTTP/1.1
    auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1", MakeHttp1HeaderLine(http::Expect, "100-continue")));
    ASSERT_EQ(st, http::StatusCodeOK);
    EXPECT_FALSE(callWantClose());
    EXPECT_TRUE(req.hasExpectContinue());
  }
  {  // Expect header on HTTP/1.0 should be ignored
    auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.0", MakeHttp1HeaderLine(http::Expect, "100-continue")));
    ASSERT_EQ(st, http::StatusCodeOK);
    EXPECT_FALSE(req.hasExpectContinue());
  }
}

TEST_F(HttpRequestTest, EndSetsSpanAttributesAndEnds) {
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

TEST_F(HttpRequestTest, RangeOverrideKeepsLast) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "Range: bytes=0-99\r\n"
                            "Range: bytes=100-199\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty(http::Range), "bytes=100-199");
}

TEST_F(HttpRequestTest, DuplicateContentLengthProduces400) {
  auto st = reqSet(BuildRaw("POST", "/p", "HTTP/1.1",
                            "Content-Length: 5\r\n"
                            "Content-Length: 5\r\n"));
  EXPECT_EQ(st, http::StatusCodeBadRequest);
}

TEST_F(HttpRequestTest, DuplicateHostProduces400) {
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

  uint32_t u32() { return static_cast<uint32_t>(_dist(_gen)) | (static_cast<uint32_t>(_dist(_gen)) << 8); }

  std::size_t range(std::size_t lo, std::size_t hi) {
    if (lo >= hi) {
      return lo;
    }
    return lo + (u32() % (hi - lo));
  }

  bool coin() { return (byte() & 1) != 0; }

 private:
  std::mt19937_64 _gen;
  std::uniform_int_distribution<uint16_t> _dist{0, 255};
};

// Generate a random buffer of given size
RawChars RandomBuffer(FuzzRng& rng, std::size_t size) {
  RawChars buf;
  buf.reserve(size);
  for (std::size_t ii = 0; ii < size; ++ii) {
    buf.push_back(static_cast<char>(rng.byte()));
  }
  return buf;
}

// Generate semi-valid HTTP request-like data
RawChars SemiValidRequest(FuzzRng& rng) {
  static constexpr std::array kMethods = {"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS", "TRACE"};
  static constexpr std::array kVersions = {"HTTP/1.0", "HTTP/1.1", "HTTP/2.0", "HTTP/0.9", "HTXP/1.1"};

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
TEST_F(HttpRequestTest, RandomBytes) {
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
TEST_F(HttpRequestTest, SemiValidRequests) {
  constexpr std::size_t kIterations = 5000;

  for (std::size_t seed = 0; seed < kIterations; ++seed) {
    FuzzRng rng(seed + 1000000);  // Different seed range
    RawChars input = SemiValidRequest(rng);
    ASSERT_NO_FATAL_FAILURE(fuzzHttpRequestParsing(input));
  }
}

// Fuzz test with mutation of valid requests
TEST_F(HttpRequestTest, MutatedValidRequests) {
  constexpr std::size_t kIterations = 5000;

  // Base valid requests to mutate
  static const std::array<std::string_view, 5> kBaseRequests = {
      "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n",
      "POST /api/data HTTP/1.1\r\nHost: api.example.com\r\nContent-Length: 0\r\n\r\n",
      "PUT /resource HTTP/1.0\r\nHost: host\r\nContent-Type: application/json\r\n\r\n",
      "DELETE /item/123 HTTP/1.1\r\nHost: h\r\nAuthorization: Bearer token\r\n\r\n",
      "GET /path?key=value&foo=bar HTTP/1.1\r\nHost: h\r\nAccept: */*\r\n\r\n",
  };

  for (std::size_t seed = 0; seed < kIterations; ++seed) {
    FuzzRng rng(seed + 2000000);

    // Pick a base request
    std::string_view base = kBaseRequests[rng.range(0, kBaseRequests.size())];
    std::string input(base);

    // Apply mutations
    std::size_t numMutations = rng.range(1, 10);
    for (std::size_t mm = 0; mm < numMutations && !input.empty(); ++mm) {
      std::size_t mutationType = rng.range(0, 5);
      std::size_t pos = rng.range(0, input.size());

      switch (mutationType) {
        case 0:  // Flip a byte
          input[pos] = static_cast<char>(input[pos] ^ static_cast<char>(rng.byte()));
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

    RawChars rawInput;
    rawInput.append(input);
    ASSERT_NO_FATAL_FAILURE(fuzzHttpRequestParsing(rawInput));
  }
}

// Fuzz test with edge case patterns
TEST_F(HttpRequestTest, EdgeCasePatterns) {
  // Collection of edge case inputs
  static const std::array<std::string_view, 16> kEdgeCases = {
      "",                                                                     // Empty
      "\r\n",                                                                 // Just CRLF
      "\r\n\r\n",                                                             // Double CRLF
      "GET",                                                                  // Incomplete
      "GET ",                                                                 // Method only
      "GET /",                                                                // No version
      "GET / HTTP/1.1",                                                       // No CRLF
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

  for (const auto& input : kEdgeCases) {
    RawChars buf;
    buf.append(input);
    ASSERT_NO_FATAL_FAILURE(fuzzHttpRequestParsing(buf));
  }

  // Also fuzz with merge disabled
  for (const auto& input : kEdgeCases) {
    RawChars buf;
    buf.append(input);
    ASSERT_NO_FATAL_FAILURE(fuzzHttpRequestParsing(buf, false));
  }
}

// Fuzz test with long headers/paths
TEST_F(HttpRequestTest, LongInputs) {
  // Long header name
  {
    std::string longName(1000, 'A');
    std::string req = "GET / HTTP/1.1\r\n" + longName + ": v\r\n\r\n";
    RawChars buf;
    buf.append(req);
    ASSERT_NO_FATAL_FAILURE(fuzzHttpRequestParsing(buf));
  }

  // Long header value
  {
    std::string longVal(10000, 'B');
    std::string req = "GET / HTTP/1.1\r\nX: " + longVal + "\r\n\r\n";
    RawChars buf;
    buf.append(req);
    ASSERT_NO_FATAL_FAILURE(fuzzHttpRequestParsing(buf));
  }

  // Long path
  {
    std::string longPath(5000, 'x');
    std::string req = "GET /" + longPath + " HTTP/1.1\r\nHost: h\r\n\r\n";
    RawChars buf;
    buf.append(req);
    ASSERT_NO_FATAL_FAILURE(fuzzHttpRequestParsing(buf));
  }
}

// Fuzz test specifically targeting header parsing
TEST_F(HttpRequestTest, HeaderParsingStress) {
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
        static constexpr std::string_view kKnownHeaders[] = {
            http::Host,       http::ContentLength,    http::ContentType,
            "Accept",         "User-Agent",           "Authorization",
            "Cookie",         "Set-Cookie",           http::CacheControl,
            http::Connection, http::TransferEncoding, http::AcceptEncoding,
            "Accept-Language"};
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
// HttpRequest::makeResponse tests
// ============================

TEST_F(HttpRequestTest, MakeResponseStatusCodeOnly) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse(http::StatusCodeAccepted);

  EXPECT_EQ(resp.status(), http::StatusCodeAccepted);
  EXPECT_TRUE(resp.bodyInMemory().empty());

  // Check that global headers are present
  EXPECT_TRUE(resp.hasHeader("server"));
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
}

TEST_F(HttpRequestTest, MakeResponseAdditionalCapacityStatusCodeOnly) {
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

TEST_F(HttpRequestTest, MakeResponseStatusCodeDefault200) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse();

  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_TRUE(resp.bodyInMemory().empty());
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
}

TEST_F(HttpRequestTest, MakeResponseBodyAndDefaultContentType) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse("Hello World");

  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.bodyInMemory(), "Hello World");
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), http::ContentTypeTextPlain);
}

TEST_F(HttpRequestTest, MakeResponseBodyAndCustomContentType) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse(R"({"key":"value"})", "application/json");

  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.bodyInMemory(), "{\"key\":\"value\"}");
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "application/json");
}

TEST_F(HttpRequestTest, MakeResponseStatusCodeBodyAndContentType) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse(http::StatusCodeCreated, "<html>OK</html>", "text/html");

  EXPECT_EQ(resp.status(), http::StatusCodeCreated);
  EXPECT_EQ(resp.bodyInMemory(), "<html>OK</html>");
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/html");
}

TEST_F(HttpRequestTest, MakeResponseBytesBodyAndDefaultContentType) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  const std::array<std::byte, 5> binaryData = {std::byte{0x48}, std::byte{0x65}, std::byte{0x6c}, std::byte{0x6c},
                                               std::byte{0x6f}};
  auto resp = req.makeResponse(std::span<const std::byte>{binaryData});

  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.bodyInMemory(), "Hello");
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), http::ContentTypeApplicationOctetStream);
}

TEST_F(HttpRequestTest, MakeResponseBytesBodyAndCustomContentType) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  const std::array<std::byte, 8> pngHeader = {std::byte{0x89}, std::byte{0x50}, std::byte{0x4e}, std::byte{0x47},
                                              std::byte{0x0d}, std::byte{0x0a}, std::byte{0x1a}, std::byte{0x0a}};
  auto resp = req.makeResponse(std::span<const std::byte>{pngHeader}, "image/png");

  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.bodyInMemory().size(), 8UL);
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "image/png");
}

TEST_F(HttpRequestTest, MakeResponseStatusCodeBytesBodyAndContentType) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  const std::array<std::byte, 4> data = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
  auto resp = req.makeResponse(http::StatusCodePartialContent, std::span<const std::byte>{data}, "application/binary");

  EXPECT_EQ(resp.status(), http::StatusCodePartialContent);
  EXPECT_EQ(resp.bodyInMemory().size(), 4UL);
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "application/binary");
}

TEST_F(HttpRequestTest, MakeResponseCanBeModifiedAfterCreation) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse(http::StatusCodeOK, "initial");
  resp.header("X-Custom", "value");
  resp.header("X-Another", "data");

  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
  EXPECT_EQ(resp.headerValueOrEmpty("X-Custom"), "value");
  EXPECT_EQ(resp.headerValueOrEmpty("X-Another"), "data");
  EXPECT_EQ(resp.bodyInMemory(), "initial");
}

TEST_F(HttpRequestTest, MakeResponseEmptyBodyStillPrefillesGlobalHeaders) {
  auto st = reqSet(BuildRaw("GET", "/test", "HTTP/1.1"));
  ASSERT_EQ(st, http::StatusCodeOK);

  auto resp = req.makeResponse(http::StatusCodeNoContent);

  EXPECT_EQ(resp.status(), http::StatusCodeNoContent);
  EXPECT_TRUE(resp.bodyInMemory().empty());
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
}

TEST_F(HttpRequestTest, MakeResponseWithMultipleGlobalHeaders) {
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

}  // namespace aeronet
