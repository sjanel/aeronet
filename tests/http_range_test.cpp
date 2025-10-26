#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/static-file-handler.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "stringconv.hpp"
#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_tls_client.hpp"
#endif

namespace {

std::string getHeader(const aeronet::test::ParsedResponse& resp, const std::string& key) {
  const auto it = resp.headers.find(key);
  if (it == resp.headers.end()) {
    return {};
  }
  return it->second;
}

}  // namespace

TEST(HttpRangeStatic, ServeCompleteFile) {
  aeronet::test::ScopedTempFile tmp("example.txt", "abcdefghij");
  const std::string fileName = tmp.filename();

  aeronet::StaticFileHandler handler(tmp.dirPath());
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;

  const auto raw = aeronet::test::requestOrThrow(ts.port(), opt);
  const auto parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).statusCode, aeronet::http::StatusCodeOK);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).body, "abcdefghij");
  EXPECT_EQ(getHeader(parsed.value_or(aeronet::test::ParsedResponse{}), "Accept-Ranges"), "bytes");
  EXPECT_FALSE(getHeader(parsed.value_or(aeronet::test::ParsedResponse{}), "ETag").empty());
  EXPECT_FALSE(getHeader(parsed.value_or(aeronet::test::ParsedResponse{}), "Last-Modified").empty());
}

TEST(HttpRangeStatic, SingleRangePartialContent) {
  aeronet::test::ScopedTempFile tmp("range.txt", "abcdefghij");
  const std::string fileName = tmp.filename();

  aeronet::StaticFileHandler handler(tmp.dirPath());
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-3");

  const auto raw = aeronet::test::requestOrThrow(ts.port(), opt);
  const auto parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).statusCode, aeronet::http::StatusCodePartialContent);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).body, "abcd");
  EXPECT_EQ(getHeader(parsed.value_or(aeronet::test::ParsedResponse{}), "Content-Range"), "bytes 0-3/10");
}

TEST(HttpRangeStatic, UnsatisfiableRange) {
  aeronet::test::ScopedTempFile tmp("range.txt", "abcdefghij");
  const std::string fileName = tmp.filename();

  aeronet::StaticFileHandler handler(tmp.dirPath());
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=100-200");

  const auto raw = aeronet::test::requestOrThrow(ts.port(), opt);
  const auto parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).statusCode, aeronet::http::StatusCodeRangeNotSatisfiable);
  EXPECT_EQ(getHeader(parsed.value_or(aeronet::test::ParsedResponse{}), "Content-Range"), "bytes */10");
}

TEST(HttpRangeStatic, IfNoneMatchReturns304) {
  aeronet::test::ScopedTempFile tmp("etag.txt", "abcdefghij");
  const std::string fileName = tmp.filename();

  aeronet::StaticFileHandler handler(tmp.dirPath());
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  aeronet::test::RequestOptions initial;
  initial.method = "GET";
  initial.target = "/" + fileName;
  const auto firstRaw = aeronet::test::requestOrThrow(ts.port(), initial);
  const auto firstParsed = aeronet::test::parseResponse(firstRaw);
  ASSERT_TRUE(firstParsed);
  const auto etag = getHeader(firstParsed.value_or(aeronet::test::ParsedResponse{}), "ETag");
  ASSERT_FALSE(etag.empty());

  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("If-None-Match", etag);

  const auto raw = aeronet::test::requestOrThrow(ts.port(), opt);
  const auto parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).statusCode, aeronet::http::StatusCodeNotModified);
  EXPECT_TRUE(parsed.value_or(aeronet::test::ParsedResponse{}).body.empty());
}

TEST(HttpRangeStatic, IfRangeMismatchFallsBackToFullBody) {
  aeronet::test::ScopedTempFile tmp("if-range.txt", "abcdefghij");
  const std::string fileName = tmp.filename();

  aeronet::StaticFileHandler handler(tmp.dirPath());
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-3");
  opt.headers.emplace_back("If-Range", "\"mismatch\"");

  const auto raw = aeronet::test::requestOrThrow(ts.port(), opt);
  const auto parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).statusCode, aeronet::http::StatusCodeOK);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).body, "abcdefghij");
}

TEST(HttpRangeInvalid, BadRangeSyntax) {
  aeronet::test::ScopedTempFile tmp("file.bin", "0123456789");
  const std::string fileName = tmp.filename();

  aeronet::StaticFileHandler handler(tmp.dirPath());
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;

  // Non-numeric start
  opt.headers.clear();
  opt.headers.emplace_back("Range", "bytes=abc-4");
  auto raw = aeronet::test::requestOrThrow(ts.port(), opt);
  auto parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).statusCode, aeronet::http::StatusCodeRangeNotSatisfiable);

  // Multiple ranges -> treated as invalid (per implementation)
  opt.headers.clear();
  opt.headers.emplace_back("Range", "bytes=0-1,2-3");
  raw = aeronet::test::requestOrThrow(ts.port(), opt);
  parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).statusCode, aeronet::http::StatusCodeRangeNotSatisfiable);

  // Suffix zero is invalid (bytes=-0)
  opt.headers.clear();
  opt.headers.emplace_back("Range", "bytes=-0");
  raw = aeronet::test::requestOrThrow(ts.port(), opt);
  parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).statusCode, aeronet::http::StatusCodeRangeNotSatisfiable);
}

TEST(HttpRangeInvalid, ConditionalInvalidDates) {
  aeronet::test::ScopedTempFile tmp("file.txt", "hello world");
  const std::string fileName = tmp.filename();

  aeronet::StaticFileHandler handler(tmp.dirPath());
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;

  // If-Modified-Since with an invalid date should be ignored -> full body returned
  opt.headers.clear();
  opt.headers.emplace_back("If-Modified-Since", "Not a date");
  auto raw = aeronet::test::requestOrThrow(ts.port(), opt);
  auto parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).statusCode, aeronet::http::StatusCodeOK);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).body, "hello world");

  // If-Unmodified-Since invalid date should be ignored (no 412)
  opt.headers.clear();
  opt.headers.emplace_back("If-Unmodified-Since", "garbage-date");
  raw = aeronet::test::requestOrThrow(ts.port(), opt);
  parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).statusCode, aeronet::http::StatusCodeOK);
}

TEST(HttpRangeInvalid, IfMatchPreconditionFailed) {
  aeronet::test::ScopedTempFile tmp("file.txt", "HELLO");
  const std::string fileName = tmp.filename();

  aeronet::StaticFileHandler handler(tmp.dirPath());
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  // First fetch to get ETag
  aeronet::test::RequestOptions initial;
  initial.method = "GET";
  initial.target = "/" + fileName;
  const auto firstRaw = aeronet::test::requestOrThrow(ts.port(), initial);
  const auto firstParsed = aeronet::test::parseResponse(firstRaw);
  ASSERT_TRUE(firstParsed);
  const auto headers = firstParsed.value_or(aeronet::test::ParsedResponse{}).headers;
  const auto etag = headers.find("ETag");
  ASSERT_NE(etag, headers.end());

  // If-Match with a non-matching tag -> 412 Precondition Failed
  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("If-Match", "\"no-match\"");

  auto raw = aeronet::test::requestOrThrow(ts.port(), opt);
  auto parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).statusCode, aeronet::http::StatusCodePreconditionFailed);
}

TEST(HttpLargeFile, ServeLargeFile) {
  const std::uint64_t size = 16ULL * 1024ULL * 1024ULL;
  aeronet::test::ScopedTempFile tmp("big.bin", size);
  const auto fileName = tmp.filename();
  const auto& data = tmp.content();

  aeronet::StaticFileHandler handler(tmp.dirPath());
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  // Use a custom connection to manually control receive behavior for large files
  aeronet::test::ClientConnection cnx(ts.port());
  int fd = cnx.fd();

  std::string req = "GET /" + fileName + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(fd, req));

  // Use recvWithTimeout which waits for complete Content-Length
  const auto raw = aeronet::test::recvWithTimeout(fd, std::chrono::seconds(10));

  const auto parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).statusCode, aeronet::http::StatusCodeOK);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).body.size(), size);
  const auto headers = parsed.value_or(aeronet::test::ParsedResponse{}).headers;
  const auto it = headers.find("Content-Length");
  ASSERT_NE(it, headers.end());
  EXPECT_EQ(aeronet::StringToIntegral<std::uint64_t>(it->second), size);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).body, data);
}

#ifdef AERONET_ENABLE_OPENSSL
TEST(HttpLargeFile, ServeLargeFileTls) {
  const std::uint64_t size = 16ULL * 1024ULL * 1024ULL;
  aeronet::test::ScopedTempFile tmp("big-tls.bin", size);
  const auto fileName = tmp.filename();
  const auto& data = tmp.content();

  aeronet::StaticFileHandler handler(tmp.dirPath());
  aeronet::test::TlsTestServer ts({"http/1.1"});
  ts.setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  aeronet::test::TlsClient client(ts.port());
  const auto raw = client.get("/" + fileName, {});
  ts.stop();

  const auto parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).statusCode, aeronet::http::StatusCodeOK);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).body.size(), size);
  const auto headers = parsed.value_or(aeronet::test::ParsedResponse{}).headers;
  const auto it = headers.find("Content-Length");
  ASSERT_NE(it, headers.end());
  EXPECT_EQ(aeronet::StringToIntegral<std::uint64_t>(it->second), size);
  EXPECT_EQ(parsed.value_or(aeronet::test::ParsedResponse{}).body, data);
}
#endif
