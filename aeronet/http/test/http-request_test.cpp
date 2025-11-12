#include "aeronet/http-request.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
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
    for (const auto &[key, val] : headers) {
      EXPECT_EQ(req.headerValueOrEmpty(key), val);
    }
  }

  HttpRequest req;
  ConnectionState cs;
};

TEST_F(HttpRequestTest, InvalidRequest) {
  EXPECT_EQ(reqSet(BuildRaw("GET", "/", "HTTP")), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet(BuildRaw("GET", "/", "HTTP/1.1", "Server: aeronet", false)), 0);
}

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

}  // namespace aeronet
