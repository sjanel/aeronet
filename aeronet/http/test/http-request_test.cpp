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

TEST(HttpRequestUnit, ParseBasicPathAndVersion) {
  HttpRequest req;
  ConnectionState cs;
  auto raw = BuildRaw("GET", "/abc", "HTTP/1.1");
  cs.buffer.assign(raw.data(), raw.data() + raw.size());
  auto st = req.setHead(cs, 4096UL);
  EXPECT_EQ(st, http::StatusCodeOK);
  EXPECT_EQ(req.method(), http::Method::GET);
  EXPECT_EQ(req.path(), "/abc");
  EXPECT_EQ(req.version(), http::HTTP_1_1);
  EXPECT_TRUE(req.queryParams().begin() == req.queryParams().end());
}

TEST(HttpRequestUnit, QueryParamsDecodingPlusAndPercent) {
  HttpRequest req;
  ConnectionState cs;
  // a=1+2&b=hello%20world&c=%zz (malformed % sequence left verbatim for c's value)
  auto raw = BuildRaw("GET", "/p?a=1+2&b=hello%20world&c=%zz");
  cs.buffer.assign(raw.data(), raw.data() + raw.size());
  auto st = req.setHead(cs, 4096UL);
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

TEST(HttpRequestUnit, EmptyAndMissingValues) {
  HttpRequest req;
  ConnectionState cs;
  auto raw = BuildRaw("GET", "/p?k1=&k2&=v");
  cs.buffer.assign(raw.data(), raw.data() + raw.size());
  auto st = req.setHead(cs, 4096UL);
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

TEST(HttpRequestUnit, DuplicateKeysPreservedOrder) {
  HttpRequest req;
  ConnectionState cs;
  auto raw = BuildRaw("GET", "/p?x=1&x=2&x=3");
  cs.buffer.assign(raw.data(), raw.data() + raw.size());
  auto st = req.setHead(cs, 4096UL);
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

TEST(HttpRequestUnit, InvalidPathEscapeCauses400) {
  HttpRequest req;
  ConnectionState cs;
  auto raw = BuildRaw("GET", "/bad%zz");
  cs.buffer.assign(raw.data(), raw.data() + raw.size());
  auto st = req.setHead(cs, 4096UL);
  EXPECT_EQ(st, http::StatusCodeBadRequest);
}

}  // namespace aeronet
