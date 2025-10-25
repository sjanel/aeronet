#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/http-status-code.hpp"
#include "aeronet/static-file.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

namespace {
class TempDir {
 public:
  TempDir() {
    const auto base = std::filesystem::temp_directory_path();
    std::size_t counter = 0;
    do {
      _path = base / ("aeronet-static-test-" + std::to_string(counter++));
    } while (std::filesystem::exists(_path));
    std::filesystem::create_directories(_path);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  TempDir(TempDir&& other) noexcept : _path(std::move(other._path)) { other._path.clear(); }
  TempDir& operator=(TempDir&& other) noexcept {
    if (this != &other) {
      cleanup();
      _path = std::move(other._path);
      other._path.clear();
    }
    return *this;
  }

  ~TempDir() { cleanup(); }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return _path; }

 private:
  void cleanup() {
    if (!_path.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(_path, ec);
    }
  }

  std::filesystem::path _path;
};

std::string writeFile(const std::filesystem::path& root, std::string_view name, std::string_view content) {
  const auto filePath = root / name;
  std::ofstream out(filePath, std::ios::binary);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.close();
  return filePath.filename().string();
}

std::string getHeader(const aeronet::test::ParsedResponse& resp, const std::string& key) {
  const auto it = resp.headers.find(key);
  if (it == resp.headers.end()) {
    return {};
  }
  return it->second;
}

}  // namespace

TEST(HttpRangeStatic, ServeCompleteFile) {
  TempDir tmp;
  const std::string fileName = writeFile(tmp.path(), "example.txt", "abcdefghij");

  aeronet::StaticFileHandler handler(tmp.path());
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;

  const auto raw = aeronet::test::requestOrThrow(ts.port(), opt);
  const auto parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->statusCode, aeronet::http::StatusCodeOK);
  EXPECT_EQ(parsed->body, "abcdefghij");
  EXPECT_EQ(getHeader(*parsed, "Accept-Ranges"), "bytes");
  EXPECT_FALSE(getHeader(*parsed, "ETag").empty());
  EXPECT_FALSE(getHeader(*parsed, "Last-Modified").empty());
}

TEST(HttpRangeStatic, SingleRangePartialContent) {
  TempDir tmp;
  const std::string fileName = writeFile(tmp.path(), "range.txt", "abcdefghij");

  aeronet::StaticFileHandler handler(tmp.path());
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-3");

  const auto raw = aeronet::test::requestOrThrow(ts.port(), opt);
  const auto parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->statusCode, aeronet::http::StatusCodePartialContent);
  EXPECT_EQ(parsed->body, "abcd");
  EXPECT_EQ(getHeader(*parsed, "Content-Range"), "bytes 0-3/10");
}

TEST(HttpRangeStatic, UnsatisfiableRange) {
  TempDir tmp;
  const std::string fileName = writeFile(tmp.path(), "range.txt", "abcdefghij");

  aeronet::StaticFileHandler handler(tmp.path());
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=100-200");

  const auto raw = aeronet::test::requestOrThrow(ts.port(), opt);
  const auto parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->statusCode, aeronet::http::StatusCodeRangeNotSatisfiable);
  EXPECT_EQ(getHeader(*parsed, "Content-Range"), "bytes */10");
}

TEST(HttpRangeStatic, IfNoneMatchReturns304) {
  TempDir tmp;
  const std::string fileName = writeFile(tmp.path(), "etag.txt", "abcdefghij");

  aeronet::StaticFileHandler handler(tmp.path());
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  aeronet::test::RequestOptions initial;
  initial.method = "GET";
  initial.target = "/" + fileName;
  const auto firstRaw = aeronet::test::requestOrThrow(ts.port(), initial);
  const auto firstParsed = aeronet::test::parseResponse(firstRaw);
  ASSERT_TRUE(firstParsed);
  const auto etag = getHeader(*firstParsed, "ETag");
  ASSERT_FALSE(etag.empty());

  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("If-None-Match", etag);

  const auto raw = aeronet::test::requestOrThrow(ts.port(), opt);
  const auto parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->statusCode, aeronet::http::StatusCodeNotModified);
  EXPECT_TRUE(parsed->body.empty());
}

TEST(HttpRangeStatic, IfRangeMismatchFallsBackToFullBody) {
  TempDir tmp;
  const std::string fileName = writeFile(tmp.path(), "if-range.txt", "abcdefghij");

  aeronet::StaticFileHandler handler(tmp.path());
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
  EXPECT_EQ(parsed->statusCode, aeronet::http::StatusCodeOK);
  EXPECT_EQ(parsed->body, "abcdefghij");
}

TEST(HttpRangeInvalid, BadRangeSyntax) {
  const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "aeronet-range-invalid";
  std::error_code ec;
  std::filesystem::create_directories(tmp, ec);
  const std::string fileName = writeFile(tmp, "file.bin", "0123456789");

  aeronet::StaticFileHandler handler(tmp);
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
  EXPECT_EQ(parsed->statusCode, aeronet::http::StatusCodeRangeNotSatisfiable);

  // Multiple ranges -> treated as invalid (per implementation)
  opt.headers.clear();
  opt.headers.emplace_back("Range", "bytes=0-1,2-3");
  raw = aeronet::test::requestOrThrow(ts.port(), opt);
  parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->statusCode, aeronet::http::StatusCodeRangeNotSatisfiable);

  // Suffix zero is invalid (bytes=-0)
  opt.headers.clear();
  opt.headers.emplace_back("Range", "bytes=-0");
  raw = aeronet::test::requestOrThrow(ts.port(), opt);
  parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->statusCode, aeronet::http::StatusCodeRangeNotSatisfiable);
}

TEST(HttpRangeInvalid, ConditionalInvalidDates) {
  const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "aeronet-range-invalid-dates";
  std::error_code ec;
  std::filesystem::create_directories(tmp, ec);
  const std::string fileName = writeFile(tmp, "file.txt", "hello world");

  aeronet::StaticFileHandler handler(tmp);
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
  EXPECT_EQ(parsed->statusCode, aeronet::http::StatusCodeOK);
  EXPECT_EQ(parsed->body, "hello world");

  // If-Unmodified-Since invalid date should be ignored (no 412)
  opt.headers.clear();
  opt.headers.emplace_back("If-Unmodified-Since", "garbage-date");
  raw = aeronet::test::requestOrThrow(ts.port(), opt);
  parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->statusCode, aeronet::http::StatusCodeOK);
}

TEST(HttpRangeInvalid, IfMatchPreconditionFailed) {
  const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "aeronet-ifmatch";
  std::error_code ec;
  std::filesystem::create_directories(tmp, ec);
  const std::string fileName = writeFile(tmp, "file.txt", "HELLO");

  aeronet::StaticFileHandler handler(tmp);
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  // First fetch to get ETag
  aeronet::test::RequestOptions initial;
  initial.method = "GET";
  initial.target = "/" + fileName;
  const auto firstRaw = aeronet::test::requestOrThrow(ts.port(), initial);
  const auto firstParsed = aeronet::test::parseResponse(firstRaw);
  ASSERT_TRUE(firstParsed);
  const auto etag = firstParsed->headers.find("ETag");
  ASSERT_NE(etag, firstParsed->headers.end());

  // If-Match with a non-matching tag -> 412 Precondition Failed
  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("If-Match", "\"no-match\"");

  auto raw = aeronet::test::requestOrThrow(ts.port(), opt);
  auto parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->statusCode, aeronet::http::StatusCodePreconditionFailed);
}

namespace {

std::filesystem::path makeTempDir() {
  const auto base = std::filesystem::temp_directory_path();
  for (int i = 0; i < 1000; ++i) {
    const auto p = base / ("aeronet-large-file-test-" +
                           std::to_string(std::uint64_t(std::hash<std::string>{}(std::to_string(i)))));
    if (!std::filesystem::exists(p)) {
      std::error_code ec;
      std::filesystem::create_directories(p, ec);
      if (!ec) return p;
    }
  }
  throw std::runtime_error("Failed to create temp dir");
}

std::string writeLargeFile(const std::filesystem::path& root, std::string_view name, std::uint64_t bytes) {
  const auto filePath = root / name;
  std::ofstream out(filePath, std::ios::binary);
  if (!out) throw std::runtime_error("failed to open file");

  const std::size_t chunkSize = 1024 * 1024;  // 1 MiB
  std::string chunk(chunkSize, 'x');
  std::uint64_t remaining = bytes;
  while (remaining >= chunkSize) {
    out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    remaining -= chunkSize;
  }
  if (remaining) {
    out.write(chunk.data(), static_cast<std::streamsize>(remaining));
  }
  out.close();
  return filePath.filename().string();
}

}  // namespace

TEST(HttpLargeFile, ServeLargeFile) {
  const auto tmp = makeTempDir();
  const std::uint64_t size = (8ULL * 1024ULL * 1024ULL) + (1ULL * 1024ULL * 1024ULL);  // 9 MiB
  const std::string fileName = writeLargeFile(tmp, "big.bin", size);

  aeronet::StaticFileHandler handler(tmp);
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([handler](const aeronet::HttpRequest& req) mutable { return handler(req); });

  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;

  const auto raw = aeronet::test::requestOrThrow(ts.port(), opt);
  const auto parsed = aeronet::test::parseResponse(raw);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->statusCode, aeronet::http::StatusCodeOK);
  EXPECT_EQ(parsed->body.size(), size);
  const auto it = parsed->headers.find("Content-Length");
  ASSERT_NE(it, parsed->headers.end());
  EXPECT_EQ(std::stoull(it->second), size);
}
