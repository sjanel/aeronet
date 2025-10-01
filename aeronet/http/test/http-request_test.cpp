#include "aeronet/http-request.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "connection-state.hpp"
#include "http-method.hpp"
#include "http-status-code.hpp"
#include "http-version.hpp"
#include "raw-chars.hpp"

namespace aeronet {

namespace {
// Helper to build a raw HTTP request buffer we can feed into HttpRequest::setHead
std::string BuildRaw(std::string_view method, std::string_view target, std::string_view version = "HTTP/1.1",
                     std::string_view extraHeaders = "") {
  std::string str;
  str.append(method).push_back(' ');
  str.append(target).push_back(' ');
  str.append(version).append("\r\n");
  str.append("Host: h\r\n");
  str.append(extraHeaders);
  str.append("\r\n");
  return str;
}
}  // namespace

class HttpRequestTest : public ::testing::Test {
 protected:
  http::StatusCode reqSet(std::size_t maxHeaderSize = 4096UL) { return req.setHead(cs, maxHeaderSize); }

  HttpRequest req;
  ConnectionState cs;
};

TEST_F(HttpRequestTest, ParseBasicPathAndVersion) {
  auto raw = BuildRaw("GET", "/abc", "HTTP/1.1");
  cs.buffer.assign(raw.data(), raw.data() + raw.size());
  auto st = reqSet();
  EXPECT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.method(), http::Method::GET);
  EXPECT_EQ(req.path(), "/abc");
  EXPECT_EQ(req.version(), http::HTTP_1_1);
  EXPECT_TRUE(req.queryParams().begin() == req.queryParams().end());
}

TEST_F(HttpRequestTest, QueryParamsDecodingPlusAndPercent) {
  // a=1+2&b=hello%20world&c=%zz (malformed % sequence left verbatim for c's value)
  auto raw = BuildRaw("GET", "/p?a=1+2&b=hello%20world&c=%zz");
  cs.buffer.assign(raw.data(), raw.data() + raw.size());
  auto st = reqSet();
  ASSERT_EQ(st, http::StatusCodeOK);
  std::vector<std::pair<std::string_view, std::string_view>> seen;
  for (auto [k, v] : req.queryParams()) {
    seen.emplace_back(k, v);
  }
  ASSERT_EQ(seen.size(), 3U);
  EXPECT_EQ(seen[0].first, "a");
  EXPECT_EQ(seen[0].second, "1 2");  // '+' => space
  EXPECT_EQ(seen[1].first, "b");
  EXPECT_EQ(seen[1].second, "hello world");  // %20 decoded
  EXPECT_EQ(seen[2].first, "c");
  EXPECT_EQ(seen[2].second, "%zz");  // invalid escape left as-is
}

TEST_F(HttpRequestTest, EmptyAndMissingValues) {
  auto raw = BuildRaw("GET", "/p?k1=&k2&=v");
  cs.buffer.assign(raw.data(), raw.data() + raw.size());
  auto st = reqSet();
  ASSERT_EQ(st, http::StatusCodeOK);
  std::vector<std::pair<std::string_view, std::string_view>> seen;
  for (auto [k, v] : req.queryParams()) {
    seen.emplace_back(k, v);
  }
  ASSERT_EQ(seen.size(), 3u);
  EXPECT_EQ(seen[0].first, "k1");
  EXPECT_EQ(seen[0].second, "");
  EXPECT_EQ(seen[1].first, "k2");
  EXPECT_EQ(seen[1].second, "");
  EXPECT_EQ(seen[2].first, "");
  EXPECT_EQ(seen[2].second, "v");
}

TEST_F(HttpRequestTest, DuplicateKeysPreservedOrder) {
  auto raw = BuildRaw("GET", "/p?x=1&x=2&x=3");
  cs.buffer.assign(raw.data(), raw.data() + raw.size());
  auto st = reqSet();
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
  cs.buffer.assign(raw.data(), raw.data() + raw.size());
  auto st = reqSet();
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
  cs.buffer.assign(raw.data(), raw.data() + raw.size());
  auto st = reqSet();
  ASSERT_EQ(st, http::StatusCodeOK);

  // Existing normal header
  EXPECT_EQ(req.headerValueOrEmpty("X-Test"), "Value");
  auto optVal = req.headerValue("X-Test");
  ASSERT_TRUE(optVal.has_value());
  EXPECT_EQ(*optVal, "Value");

  // Case-insensitive lookup
  EXPECT_EQ(req.headerValueOrEmpty("x-test"), "Value");
  EXPECT_TRUE(req.headerValue("x-test").has_value());

  // Empty header value vs missing header
  EXPECT_EQ(req.headerValueOrEmpty("X-Empty"), "");
  auto optEmpty = req.headerValue("X-Empty");
  ASSERT_TRUE(optEmpty.has_value());
  EXPECT_EQ(*optEmpty, "");

  // Trimming behavior (trailing)
  EXPECT_EQ(req.headerValueOrEmpty("X-Trim"), "value");
  EXPECT_EQ(req.headerValueOrEmpty("x-trim"), "value");
  // Trimming behavior (leading & trailing)
  EXPECT_EQ(req.headerValueOrEmpty("X-Spaces"), "abc");
  auto optSpaces = req.headerValue("X-Spaces");
  ASSERT_TRUE(optSpaces.has_value());
  EXPECT_EQ(*optSpaces, "abc");

  EXPECT_EQ(req.headerValueOrEmpty("No-Such"), std::string_view());
  EXPECT_FALSE(req.headerValue("No-Such").has_value());
}

TEST_F(HttpRequestTest, HeaderAccessorsAbsentHeaders) {
  auto raw = BuildRaw("GET", "/p");
  cs.buffer.assign(raw.data(), raw.data() + raw.size());
  auto st = reqSet();
  ASSERT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.headerValueOrEmpty("Host"), "h");  // baseline sanity
  EXPECT_EQ(req.headerValueOrEmpty("X-Unknown"), std::string_view());
  EXPECT_FALSE(req.headerValue("X-Unknown").has_value());
}

}  // namespace aeronet
