#include "aeronet/http-request.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/connection-state.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/tracing/tracer.hpp"

namespace aeronet {

namespace {
// Helper to build a raw HTTP request buffer we can feed into HttpRequest::setHead
RawChars BuildRaw(std::string_view method, std::string_view target, std::string_view version = "HTTP/1.1",
                  std::string_view extraHeaders = "", bool includeFinalCRLF = true) {
  RawChars str;
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
  http::StatusCode reqSet(RawChars raw, bool mergeAllowedForUnknownRequestHeaders = true,
                          std::size_t maxHeaderSize = 4096UL) {
    cs.inBuffer = std::move(raw);
    RawChars tmpBuffer;
    return req.initTrySetHead(cs, tmpBuffer, maxHeaderSize, mergeAllowedForUnknownRequestHeaders, nullptr);
  }

  void checkHeaders(std::initializer_list<http::HeaderView> headers) {
    for (const auto& [key, val] : headers) {
      EXPECT_EQ(req.headerValueOrEmpty(key), val);
    }
  }

  void rehash(std::size_t capacity) { req._headers.rehash(capacity); }

  void shrink_to_fit() { req.shrink_to_fit(); }

  // Helpers that exercise private internals via friendship with HttpRequest.
  void setBodyAccessAggregated() { req._bodyAccessMode = HttpRequest::BodyAccessMode::Aggregated; }
  void setBodyAccessStreamingWithBridgeNoHasMore() {
    req._bodyAccessMode = HttpRequest::BodyAccessMode::Streaming;
    static HttpRequest::BodyAccessBridge bridge;
    bridge.readChunk = [](HttpRequest&, void*, std::size_t) -> std::string_view { return {}; };
    bridge.hasMore = [](const HttpRequest&, void*) -> bool { return false; };
    req._bodyAccessBridge = &bridge;
  }

  // Test helpers that require friend access to HttpRequest private members
  struct FakeSpan : public tracing::Span {
    static inline int64_t lastStatusCode = -1;
    static inline long lastDurationUs = -1;
    static inline bool ended = false;

    void setAttribute(std::string_view key, int64_t val) noexcept override {
      if (key == "http.status_code") {
        lastStatusCode = val;
      } else if (key == "http.duration_us") {
        lastDurationUs = static_cast<long>(val);
      }
    }

    void setAttribute([[maybe_unused]] std::string_view key, [[maybe_unused]] std::string_view val) noexcept override {}

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

  void callPinHeadStorage() { req.pinHeadStorage(cs); }

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

  HttpRequest req;
  ConnectionState cs;
};

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
  for (auto [k, v] : req.queryParams()) {
    seen.emplace_back(k, v);
  }
  ASSERT_EQ(seen.size(), 3U);
  EXPECT_EQ(seen[0].name, "a");
  EXPECT_EQ(seen[0].value, "1 2");  // '+' => space
  EXPECT_EQ(seen[1].name, "b");
  EXPECT_EQ(seen[1].value, "hello world");  // %20 decoded
  EXPECT_EQ(seen[2].name, "c");
  EXPECT_EQ(seen[2].value, "%zz");  // invalid escape left as-is
}

TEST_F(HttpRequestTest, EmptyAndMissingValues) {
  auto st = reqSet(BuildRaw("GET", "/p?k1=&k2&=v"));
  ASSERT_EQ(st, http::StatusCodeOK);
  std::vector<http::HeaderView> seen;
  for (auto [k, v] : req.queryParams()) {
    seen.emplace_back(k, v);
  }
  ASSERT_EQ(seen.size(), 3U);
  EXPECT_EQ(seen[0].name, "k1");
  EXPECT_EQ(seen[0].value, "");
  EXPECT_EQ(seen[1].name, "k2");
  EXPECT_EQ(seen[1].value, "");
  EXPECT_EQ(seen[2].name, "");
  EXPECT_EQ(seen[2].value, "v");
}

TEST_F(HttpRequestTest, DuplicateKeysPreservedOrder) {
  auto st = reqSet(BuildRaw("GET", "/p?x=1&x=2&x=3"));
  ASSERT_EQ(st, http::StatusCodeOK);
  std::vector<std::string_view> values;
  for (auto [k, v] : req.queryParams()) {
    if (k == "x") {
      values.push_back(v);
    }
  }
  ASSERT_EQ(values.size(), 3U);
  EXPECT_EQ(values[0], "1");
  EXPECT_EQ(values[1], "2");
  EXPECT_EQ(values[2], "3");
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

  checkHeaders({{"X-Test", "Value"}, {"H", "v1,v2"}, {"X-Spaces", "abc"}, {"Content-Length", "0"}});
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

  rehash(100);

  const auto originalLoadfactor = req.headers().load_factor();

  auto st = reqSet(std::move(raw));
  ASSERT_EQ(st, http::StatusCodeOK);

  shrink_to_fit();

  checkHeaders({{"X-Test", "Value"},
                {"Cookie", "cookie1;cookie2;cookie3;cookie4"},
                {"X-Spaces", "abc,de,fgh"},
                {"Content-Length", "0"}});

  EXPECT_LT(originalLoadfactor, req.headers().load_factor());
}

TEST_F(HttpRequestTest, MergeConsecutiveHeadersWithSpaces) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H: v1  \r\n"
                            "H: v2\r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v1,v2"}, {"X-Spaces", "abc"}, {"Content-Length", "0"}});
}

TEST_F(HttpRequestTest, MergeNonConsecutiveHeaders) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H:v1\r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "H:v2\r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v1,v2"}, {"X-Spaces", "abc"}, {"Content-Length", "0"}});
}

TEST_F(HttpRequestTest, MergeNonConsecutiveHeadersWithSpaces) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H: v1  \r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "H: v2\r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v1,v2"}, {"X-Spaces", "abc"}, {"Content-Length", "0"}});
}

TEST_F(HttpRequestTest, MergeNonConsecutiveHeadersWithEmptyOnFirst) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H:  \r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "H:v2\r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v2"}, {"X-Spaces", "abc"}, {"Content-Length", "0"}});
}

TEST_F(HttpRequestTest, MergeNonConsecutiveHeadersWithEmptyOnSecond) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H: v1  \r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "H:\r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v1"}, {"X-Spaces", "abc"}, {"Content-Length", "0"}});
}

TEST_F(HttpRequestTest, MergeNonConsecutiveHeadersBothEmpty) {
  auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1",
                            "X-Test: Value\r\n"
                            "H:   \r\n"
                            "X-Spaces:    abc \t  \r\n"
                            "H:\r\n"
                            "content-length: 0\r\n"));
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", ""}, {"X-Spaces", "abc"}, {"Content-Length", "0"}});

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
                {"Content-Length", "0"}});
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
                {"Content-Length", "0"}});

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

TEST_F(HttpRequestTest, PinHeadStorageRemapsViews) {
  // Build raw request with a header value we can inspect pointer for
  RawChars raw = BuildRaw("GET", "/p", "HTTP/1.1", "X-Custom: original_value\r\n");
  auto st = reqSet(std::move(raw));
  ASSERT_EQ(st, http::StatusCodeOK);

  // capture original view pointer into connection inBuffer
  auto hostView = req.headerValueOrEmpty("X-Custom");
  const char* originalPtr = hostView.data();

  // now pin head storage which moves data into state.headBuffer
  callPinHeadStorage();

  // after pinning, header view should point into headBuffer (ownerState.headBuffer)
  auto pinned = req.headerValueOrEmpty("X-Custom");
  const char* pinnedPtr = pinned.data();

  ASSERT_NE(originalPtr, pinnedPtr);
  // pinned pointer should be within headBuffer data range
  const char* hb = cs.headBuffer.data();
  EXPECT_GE(pinnedPtr, hb);
  EXPECT_LT(pinnedPtr, hb + static_cast<std::ptrdiff_t>(cs.headBuffer.size()));
}

TEST_F(HttpRequestTest, WantCloseAndHasExpectContinue) {
  {  // Connection: close
    auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1", "Connection: close\r\n"));
    ASSERT_EQ(st, http::StatusCodeOK);
    EXPECT_TRUE(callWantClose());
    EXPECT_FALSE(req.hasExpectContinue());
  }
  {  // Expect: 100-continue on HTTP/1.1
    auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.1", "Expect: 100-continue\r\n"));
    ASSERT_EQ(st, http::StatusCodeOK);
    EXPECT_FALSE(callWantClose());
    EXPECT_TRUE(req.hasExpectContinue());
  }
  {  // Expect header on HTTP/1.0 should be ignored
    auto st = reqSet(BuildRaw("GET", "/p", "HTTP/1.0", "Expect: 100-continue\r\n"));
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
  EXPECT_EQ(req.headerValueOrEmpty("Range"), "bytes=100-199");
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
  constexpr std::size_t kIterations = 10000;
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
  constexpr std::size_t kIterations = 10000;

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
  constexpr std::size_t kIterations = 5000;

  for (std::size_t seed = 0; seed < kIterations; ++seed) {
    FuzzRng rng(seed + 3000000);

    RawChars input;
    input.append("GET / HTTP/1.1\r\n");

    // Generate many headers
    std::size_t numHeaders = rng.range(0, 100);
    for (std::size_t hh = 0; hh < numHeaders; ++hh) {
      // Sometimes use known header names
      if (rng.coin()) {
        static constexpr std::array kKnownHeaders = {
            "Host",           "Content-Length", "Content-Type",  "Accept",     "User-Agent",        "Authorization",
            "Cookie",         "Set-Cookie",     "Cache-Control", "Connection", "Transfer-Encoding", "Accept-Encoding",
            "Accept-Language"};
        input.append(kKnownHeaders[rng.range(0, kKnownHeaders.size())]);
      } else {
        std::size_t nameLen = rng.range(1, 30);
        for (std::size_t ii = 0; ii < nameLen; ++ii) {
          input.push_back(static_cast<char>('a' + rng.range(0, 26)));
        }
      }
      input.append(": ");

      // Value
      std::size_t valLen = rng.range(0, 100);
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

}  // namespace aeronet
