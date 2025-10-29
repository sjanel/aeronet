#include "aeronet/http-request.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "connection-state.hpp"
#include "raw-chars.hpp"

namespace aeronet {

namespace {
// Helper to build a raw HTTP request buffer we can feed into HttpRequest::setHead
std::string BuildRaw(std::string_view method, std::string_view target, std::string_view version = "HTTP/1.1",
                     std::string_view extraHeaders = "") {
  std::string str;
  str.append(method).push_back(' ');
  str.append(target).push_back(' ');
  str.append(version).append(http::CRLF);
  str.append("Host: h");
  str.append(http::CRLF);
  str.append(extraHeaders);
  str.append(http::CRLF);
  return str;
}
}  // namespace

class HttpRequestTest : public ::testing::Test {
 protected:
  http::StatusCode reqSet(std::string_view str, bool mergeAllowedForUnknownRequestHeaders = true,
                          std::size_t maxHeaderSize = 4096UL) {
    cs.inBuffer.assign(str.data(), str.size());
    RawChars tmpBuffer;
    return req.initTrySetHead(cs, tmpBuffer, maxHeaderSize, mergeAllowedForUnknownRequestHeaders);
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
  EXPECT_EQ(reqSet("GET / HTTP\r\n"), http::StatusCodeBadRequest);
  EXPECT_EQ(reqSet("GET / HTTP1.1"), 0);
}

TEST_F(HttpRequestTest, ParseBasicPathAndVersion) {
  auto raw = BuildRaw("GET", "/abc", "HTTP/1.1");
  auto st = reqSet(raw);
  EXPECT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.method(), http::Method::GET);
  EXPECT_EQ(req.path(), "/abc");
  EXPECT_EQ(req.version(), http::HTTP_1_1);
  EXPECT_TRUE(req.queryParams().begin() == req.queryParams().end());
}

TEST_F(HttpRequestTest, QueryParamsDecodingPlusAndPercent) {
  // a=1+2&b=hello%20world&c=%zz (malformed % sequence left verbatim for c's value)
  auto raw = BuildRaw("GET", "/p?a=1+2&b=hello%20world&c=%zz");
  auto st = reqSet(raw);
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
  auto raw = BuildRaw("GET", "/p?k1=&k2&=v");
  auto st = reqSet(raw);
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
  auto raw = BuildRaw("GET", "/p?x=1&x=2&x=3");
  auto st = reqSet(raw);
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
  auto raw = BuildRaw("GET", "/bad%zz");
  auto st = reqSet(raw);
  EXPECT_EQ(st, http::StatusCodeBadRequest);
}

TEST_F(HttpRequestTest, HeaderAccessorsBasicAndEmptyVsMissing) {
  // Provide headers including:
  //  - normal value (X-Test)
  //  - empty value (X-Empty)
  //  - value with trailing spaces (X-Trim)
  //  - value with leading & trailing mixed whitespace (X-Spaces)
  //  - lowercase key to verify case-insensitive lookup (content-length)
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "X-Test: Value\r\n"
                      "X-Empty:\r\n"
                      "X-Trim: value   \r\n"
                      "X-Spaces:    abc \t  \r\n"
                      "content-length: 0\r\n");
  auto st = reqSet(raw);
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
  auto raw = BuildRaw("GET", "/p");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("Host"), "h");  // baseline sanity
  EXPECT_EQ(req.headerValueOrEmpty("X-Unknown"), std::string_view());
  EXPECT_FALSE(req.headerValue("X-Unknown").has_value());
}

TEST_F(HttpRequestTest, MergeConsecutiveHeaders) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "X-Test: Value\r\n"
                      "H:v1\r\n"
                      "H:v2\r\n"
                      "X-Spaces:    abc \t  \r\n"
                      "content-length: 0\r\n");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v1,v2"}, {"X-Spaces", "abc"}, {"Content-Length", "0"}});
}

TEST_F(HttpRequestTest, MergeConsecutiveHeadersWithSpaces) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "X-Test: Value\r\n"
                      "H: v1  \r\n"
                      "H: v2\r\n"
                      "X-Spaces:    abc \t  \r\n"
                      "content-length: 0\r\n");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v1,v2"}, {"X-Spaces", "abc"}, {"Content-Length", "0"}});
}

TEST_F(HttpRequestTest, MergeNonConsecutiveHeaders) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "X-Test: Value\r\n"
                      "H:v1\r\n"
                      "X-Spaces:    abc \t  \r\n"
                      "H:v2\r\n"
                      "content-length: 0\r\n");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v1,v2"}, {"X-Spaces", "abc"}, {"Content-Length", "0"}});
}

TEST_F(HttpRequestTest, MergeNonConsecutiveHeadersWithSpaces) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "X-Test: Value\r\n"
                      "H: v1  \r\n"
                      "X-Spaces:    abc \t  \r\n"
                      "H: v2\r\n"
                      "content-length: 0\r\n");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v1,v2"}, {"X-Spaces", "abc"}, {"Content-Length", "0"}});
}

TEST_F(HttpRequestTest, MergeNonConsecutiveHeadersWithEmptyOnFirst) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "X-Test: Value\r\n"
                      "H:  \r\n"
                      "X-Spaces:    abc \t  \r\n"
                      "H:v2\r\n"
                      "content-length: 0\r\n");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v2"}, {"X-Spaces", "abc"}, {"Content-Length", "0"}});
}

TEST_F(HttpRequestTest, MergeNonConsecutiveHeadersWithEmptyOnSecond) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "X-Test: Value\r\n"
                      "H: v1  \r\n"
                      "X-Spaces:    abc \t  \r\n"
                      "H:\r\n"
                      "content-length: 0\r\n");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", "v1"}, {"X-Spaces", "abc"}, {"Content-Length", "0"}});
}

TEST_F(HttpRequestTest, MergeNonConsecutiveHeadersBothEmpty) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "X-Test: Value\r\n"
                      "H:   \r\n"
                      "X-Spaces:    abc \t  \r\n"
                      "H:\r\n"
                      "content-length: 0\r\n");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);

  checkHeaders({{"X-Test", "Value"}, {"H", ""}, {"X-Spaces", "abc"}, {"Content-Length", "0"}});

  EXPECT_TRUE(req.headerValue("H").has_value());
}

TEST_F(HttpRequestTest, MergeMultipleCookies) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "X-Test: Value\r\n"
                      "Cookie:  cookie1 \r\n"
                      "X-Spaces:    abc \t  \r\n"
                      "Cookie:\r\n"
                      "Cookie:cookie2\r\n"
                      "Cookie:cookie3\r\n"
                      "content-length: 0\r\n"
                      "Cookie: cookie4\r\n");
  auto st = reqSet(raw);
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
  st = reqSet(raw, false);
  ASSERT_EQ(st, http::StatusCodeBadRequest);
}

TEST_F(HttpRequestTest, AcceptHeaderCommaMerge) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "Accept: text/plain\r\n"
                      "Accept: text/html\r\n");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("Accept"), "text/plain,text/html");
}

TEST_F(HttpRequestTest, AcceptHeaderSkipEmptySecond) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "Accept: text/plain\r\n"
                      "Accept:   \r\n");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("Accept"), "text/plain");
}

TEST_F(HttpRequestTest, AcceptHeaderEmptyFirstTakesSecond) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "Accept:    \r\n"
                      "Accept: text/html\r\n");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("Accept"), "text/html");
}

TEST_F(HttpRequestTest, UserAgentSpaceMerge) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "User-Agent: Foo  \r\n"
                      "User-Agent:   Bar   \r\n");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("User-Agent"), "Foo Bar");
}

TEST_F(HttpRequestTest, AuthorizationOverrideKeepsLast) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "Authorization: Bearer first\r\n"
                      "Authorization: Bearer second\r\n");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("Authorization"), "Bearer second");
}

TEST_F(HttpRequestTest, AuthorizationEmptyFirstThenValue) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "Authorization:   \r\n"
                      "Authorization: Bearer token\r\n");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("Authorization"), "Bearer token");
}

TEST_F(HttpRequestTest, AuthorizationOverrideCaseInsensitive) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "aUtHoRiZaTiOn: Bearer First\r\n"
                      "AUTHORIZATION: Bearer Second\r\n");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("Authorization"), "Bearer Second");
}

TEST_F(HttpRequestTest, RangeOverrideKeepsLast) {
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1",
                      "Range: bytes=0-99\r\n"
                      "Range: bytes=100-199\r\n");
  auto st = reqSet(raw);
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("Range"), "bytes=100-199");
}

TEST_F(HttpRequestTest, DuplicateContentLengthProduces400) {
  auto raw = BuildRaw("POST", "/p", "HTTP/1.1",
                      "Content-Length: 5\r\n"
                      "Content-Length: 5\r\n");
  auto st = reqSet(raw);
  EXPECT_EQ(st, http::StatusCodeBadRequest);
}

TEST_F(HttpRequestTest, DuplicateHostProduces400) {
  // BuildRaw already injects one Host header; we append another duplicate -> 400
  auto raw = BuildRaw("GET", "/p", "HTTP/1.1", "Host: other\r\n");
  auto st = reqSet(raw);
  EXPECT_EQ(st, http::StatusCodeBadRequest);
}

}  // namespace aeronet
