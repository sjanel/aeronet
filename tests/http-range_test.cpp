#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

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

using namespace aeronet;

namespace {

std::string getHeader(const test::ParsedResponse& resp, std::string_view key) {
  const auto it = resp.headers.find(key);
  if (it == resp.headers.end()) {
    return {};
  }
  return it->second;
}

test::TestServer ts(HttpServerConfig{});

}  // namespace

TEST(HttpRangeStatic, ServeCompleteFile) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  EXPECT_EQ(parsed.body, "abcdefghij");
  EXPECT_EQ(getHeader(parsed, "Accept-Ranges"), "bytes");
  EXPECT_FALSE(getHeader(parsed, "ETag").empty());
  EXPECT_FALSE(getHeader(parsed, "Last-Modified").empty());
}

TEST(HttpRangeStatic, SingleRangePartialContent) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-3");

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePartialContent);
  EXPECT_EQ(parsed.body, "abcd");
  EXPECT_EQ(getHeader(parsed, "Content-Range"), "bytes 0-3/10");
}

TEST(HttpRangeStatic, UnsatisfiableRange) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=100-200");

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeRangeNotSatisfiable);
  EXPECT_EQ(getHeader(parsed, "Content-Range"), "bytes */10");
}

TEST(HttpRangeStatic, IfNoneMatchReturns304) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions initial;
  initial.method = "GET";
  initial.target = "/" + fileName;
  const auto firstRaw = test::requestOrThrow(ts.port(), initial);
  const auto firstParsed = test::parseResponseOrThrow(firstRaw);
  const auto etag = getHeader(firstParsed, "ETag");
  ASSERT_FALSE(etag.empty());

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("If-None-Match", etag);

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeNotModified);
  EXPECT_TRUE(parsed.body.empty());
}

TEST(HttpRangeStatic, IfRangeMismatchFallsBackToFullBody) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-3");
  opt.headers.emplace_back("If-Range", "\"mismatch\"");

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  EXPECT_EQ(parsed.body, "abcdefghij");
}

TEST(HttpRangeInvalid, BadRangeSyntax) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "0123456789");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;

  // Non-numeric start
  opt.headers.clear();
  opt.headers.emplace_back("Range", "bytes=abc-4");
  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeRangeNotSatisfiable);

  // Multiple ranges -> treated as invalid (per implementation)
  opt.headers.clear();
  opt.headers.emplace_back("Range", "bytes=0-1,2-3");
  raw = test::requestOrThrow(ts.port(), opt);
  parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeRangeNotSatisfiable);

  // Suffix zero is invalid (bytes=-0)
  opt.headers.clear();
  opt.headers.emplace_back("Range", "bytes=-0");
  raw = test::requestOrThrow(ts.port(), opt);
  parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeRangeNotSatisfiable);
}

TEST(HttpRangeInvalid, ConditionalInvalidDates) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "hello world");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;

  // If-Modified-Since with an invalid date should be ignored -> full body returned
  opt.headers.clear();
  opt.headers.emplace_back("If-Modified-Since", "Not a date");
  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  EXPECT_EQ(parsed.body, "hello world");

  // If-Unmodified-Since invalid date should be ignored (no 412)
  opt.headers.clear();
  opt.headers.emplace_back("If-Unmodified-Since", "garbage-date");
  raw = test::requestOrThrow(ts.port(), opt);
  parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
}

TEST(HttpRangeInvalid, IfMatchPreconditionFailed) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "HELLO");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  // First fetch to get ETag
  test::RequestOptions initial;
  initial.method = "GET";
  initial.target = "/" + fileName;
  const auto firstRaw = test::requestOrThrow(ts.port(), initial);
  const auto firstParsed = test::parseResponseOrThrow(firstRaw);
  const auto headers = firstParsed.headers;
  const auto etag = headers.find("ETag");
  ASSERT_NE(etag, headers.end());

  // If-Match with a non-matching tag -> 412 Precondition Failed
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("If-Match", "\"no-match\"");

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePreconditionFailed);
}

TEST(HttpLargeFile, ServeLargeFile) {
  const std::uint64_t size = 16ULL * 1024ULL * 1024ULL;
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp([&]() {
    std::string data;
    data.assign(static_cast<std::size_t>(size), '\0');
    for (std::uint64_t i = 0; i < size; ++i) {
      data[static_cast<std::size_t>(i)] = static_cast<char>('a' + (i % 26));
    }
    return test::ScopedTempFile(tmpDir, data);
  }());
  const auto fileName = tmp.filename();
  const std::string_view data = tmp.content();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  // Use a custom connection to manually control receive behavior for large files
  test::ClientConnection cnx(ts.port());
  int fd = cnx.fd();

  std::string req = "GET /" + fileName + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);

  // Use recvWithTimeout which waits for complete Content-Length
  const auto raw = test::recvWithTimeout(fd, std::chrono::seconds(10));

  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  EXPECT_EQ(parsed.body.size(), size);
  const auto headers = parsed.headers;
  const auto it = headers.find("Content-Length");
  ASSERT_NE(it, headers.end());
  EXPECT_EQ(StringToIntegral<std::uint64_t>(it->second), size);
  EXPECT_TRUE(parsed.body == data);
}

#ifdef AERONET_ENABLE_OPENSSL
TEST(HttpLargeFile, ServeLargeFileTls) {
  const std::uint64_t size = 16ULL * 1024ULL * 1024ULL;
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp([&]() {
    std::string data;
    data.assign(static_cast<std::size_t>(size), '\0');
    for (std::uint64_t i = 0; i < size; ++i) {
      data[static_cast<std::size_t>(i)] = static_cast<char>('a' + (i % 26));
    }
    return test::ScopedTempFile(tmpDir, data);
  }());
  const auto fileName = tmp.filename();
  const std::string_view data = tmp.content();

  test::TlsTestServer tlsServer({"http/1.1"});
  tlsServer.setDefault(StaticFileHandler(tmp.dirPath()));

  test::TlsClient client(tlsServer.port());
  const auto raw = client.get("/" + fileName, {});
  tlsServer.stop();

  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  EXPECT_EQ(parsed.body.size(), size);
  const auto headers = parsed.headers;
  const auto it = headers.find("Content-Length");
  ASSERT_NE(it, headers.end());
  EXPECT_EQ(StringToIntegral<std::uint64_t>(it->second), size);
  // Compare content without printing huge data on failure
  const auto& body = parsed.body;
  EXPECT_TRUE(body == data) << "Body content mismatch (size: " << body.size() << " bytes)";
}
#endif
