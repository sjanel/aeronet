#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/static-file-handler.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/vector.hpp"

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
  EXPECT_EQ(getHeader(parsed, http::AcceptRanges), "bytes");
  EXPECT_FALSE(getHeader(parsed, http::ETag).empty());
  EXPECT_FALSE(getHeader(parsed, http::LastModified).empty());
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
  EXPECT_EQ(getHeader(parsed, http::ContentRange), "bytes 0-3/10");
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
  EXPECT_EQ(getHeader(parsed, http::ContentRange), "bytes */10");
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
  const auto etag = getHeader(firstParsed, http::ETag);
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

  // Multiple adjacent ranges → now coalesced to single valid range (bytes 0-3)
  opt.headers.clear();
  opt.headers.emplace_back("Range", "bytes=0-1,2-3");
  raw = test::requestOrThrow(ts.port(), opt);
  parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePartialContent);
  EXPECT_EQ(getHeader(parsed, http::ContentRange), "bytes 0-3/10");
  EXPECT_EQ(parsed.body, "0123");

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
  const auto etag = headers.find(http::ETag);
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
  NativeHandle fd = cnx.fd();

  std::string req = "GET /" + fileName + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);

  // Use recvWithTimeout which waits for complete Content-Length
  const auto raw = test::recvWithTimeout(fd, std::chrono::seconds(10));

  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  EXPECT_EQ(parsed.body.size(), size);
  const auto headers = parsed.headers;
  const auto it = headers.find(http::ContentLength);
  ASSERT_NE(it, headers.end());
  EXPECT_EQ(StringToIntegral<std::uint64_t>(it->second), size);
  EXPECT_TRUE(parsed.body == data);
}

#ifdef AERONET_ENABLE_OPENSSL
TEST(HttpLargeFile, ServeLargeFileTls) {
  static constexpr std::uint64_t size = 16ULL * 1024ULL * 1024ULL;
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
  const auto it = headers.find(http::ContentLength);
  ASSERT_NE(it, headers.end());
  EXPECT_EQ(StringToIntegral<std::uint64_t>(it->second), size);
  // Compare content without printing huge data on failure
  const auto& body = parsed.body;
  EXPECT_TRUE(body == data) << "Body content mismatch (size: " << body.size() << " bytes)";
}
#endif

// ---------------------------------------------------------------------------
// Multipart / multi-range integration tests (RFC 7233 multipart/byteranges)
// ---------------------------------------------------------------------------

namespace {

struct MultipartPart {
  std::string contentType;
  std::string contentRange;
  std::string body;
};

vector<MultipartPart> ParseMultipartByterangesResponse(const std::string& ctHeader, const std::string& body) {
  auto bpos = ctHeader.find("boundary=");
  if (bpos == std::string::npos) {
    return {};
  }
  std::string boundary = ctHeader.substr(bpos + 9);

  std::string delim = "\r\n--" + boundary;

  vector<MultipartPart> parts;
  std::string_view remaining = body;

  auto firstPos = remaining.find(delim);
  if (firstPos == std::string_view::npos) {
    return {};
  }
  remaining.remove_prefix(firstPos + delim.size());

  while (true) {
    if (remaining.starts_with("--")) {
      break;
    }
    if (!remaining.starts_with(http::CRLF)) {
      break;
    }
    remaining.remove_prefix(http::CRLF.size());

    auto headerEnd = remaining.find(http::DoubleCRLF);
    if (headerEnd == std::string_view::npos) {
      break;
    }
    auto headerBlock = remaining.substr(0, headerEnd);
    remaining.remove_prefix(headerEnd + http::DoubleCRLF.size());

    MultipartPart part;
    while (!headerBlock.empty()) {
      auto lineEnd = headerBlock.find(http::CRLF);
      auto line = (lineEnd == std::string_view::npos) ? headerBlock : headerBlock.substr(0, lineEnd);
      auto colon = line.find(':');
      if (colon != std::string_view::npos) {
        auto key = line.substr(0, colon);
        auto val = line.substr(colon + 1);
        while (!val.empty() && val.front() == ' ') {
          val.remove_prefix(1);
        }
        if (key == "Content-Type") {
          part.contentType = std::string(val);
        } else if (key == "Content-Range") {
          part.contentRange = std::string(val);
        }
      }
      if (lineEnd == std::string_view::npos) {
        break;
      }
      headerBlock.remove_prefix(lineEnd + http::CRLF.size());
    }

    auto nextDelim = remaining.find(delim);
    if (nextDelim == std::string_view::npos) {
      break;
    }
    part.body = std::string(remaining.substr(0, nextDelim));
    remaining.remove_prefix(nextDelim + delim.size());
    parts.push_back(std::move(part));
  }
  return parts;
}

}  // namespace

TEST(HttpRangeMulti, MultiRangePartialContent) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-3, 6-9");

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePartialContent);

  const auto ct = getHeader(parsed, http::ContentType);
  EXPECT_TRUE(ct.starts_with("multipart/byteranges; boundary=")) << "Got: " << ct;

  auto parts = ParseMultipartByterangesResponse(ct, parsed.body);
  ASSERT_EQ(parts.size(), 2U);
  EXPECT_EQ(parts[0].body, "abcd");
  EXPECT_EQ(parts[1].body, "ghij");
  EXPECT_EQ(parts[0].contentRange, "bytes 0-3/10");
  EXPECT_EQ(parts[1].contentRange, "bytes 6-9/10");
}

TEST(HttpRangeMulti, MultiRangeBodyPartsMatchFileContent) {
  test::ScopedTempDir tmpDir;
  const std::string content = "The quick brown fox jumps over the lazy dog";
  test::ScopedTempFile tmp(tmpDir, content);
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-2, 10-14, 35-42");

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePartialContent);

  auto parts = ParseMultipartByterangesResponse(getHeader(parsed, http::ContentType), parsed.body);
  ASSERT_EQ(parts.size(), 3U);
  EXPECT_EQ(parts[0].body, "The");
  EXPECT_EQ(parts[1].body, "brown");
  EXPECT_EQ(parts[2].body, "lazy dog");
}

TEST(HttpRangeMulti, MultiRangeCoalescedResponse) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-5, 3-9");  // overlapping → coalesced to 0-9

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePartialContent);
  // Coalesced to single range → simple Content-Range
  EXPECT_EQ(getHeader(parsed, http::ContentRange), "bytes 0-9/10");
  EXPECT_EQ(parsed.body, "abcdefghij");
}

TEST(HttpRangeMulti, MultiRangeSingleSatisfiable) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-3, 100-200");  // second unsatisfiable

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePartialContent);
  EXPECT_EQ(getHeader(parsed, http::ContentRange), "bytes 0-3/10");
  EXPECT_EQ(parsed.body, "abcd");
}

TEST(HttpRangeMulti, MultiRangeIfRangeInteraction) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  // First get the ETag
  test::RequestOptions initial;
  initial.method = "GET";
  initial.target = "/" + fileName;
  const auto firstRaw = test::requestOrThrow(ts.port(), initial);
  const auto firstParsed = test::parseResponseOrThrow(firstRaw);
  const auto etag = getHeader(firstParsed, http::ETag);
  ASSERT_FALSE(etag.empty());

  // Multi-range with matching If-Range → 206 multipart
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-3, 6-9");
  opt.headers.emplace_back("If-Range", etag);

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePartialContent);
  EXPECT_TRUE(getHeader(parsed, http::ContentType).starts_with("multipart/byteranges"));

  // Multi-range with mismatched If-Range → 200 full body
  opt.headers.clear();
  opt.headers.emplace_back("Range", "bytes=0-3, 6-9");
  opt.headers.emplace_back("If-Range", "\"mismatch\"");

  raw = test::requestOrThrow(ts.port(), opt);
  parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  EXPECT_EQ(parsed.body, "abcdefghij");
}

TEST(HttpRangeMulti, MultiRangeLargeFile) {
  // File larger than inlineFileThresholdBytes
  test::ScopedTempDir tmpDir;
  std::string content(256UL * 1024, '\0');  // 256 KiB
  for (std::size_t i = 0; i < content.size(); ++i) {
    content[i] = static_cast<char>('a' + (i % 26));
  }
  test::ScopedTempFile tmp(tmpDir, content);
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-9, 1000-1009, 100000-100009");

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePartialContent);

  auto parts = ParseMultipartByterangesResponse(getHeader(parsed, http::ContentType), parsed.body);
  ASSERT_EQ(parts.size(), 3U);
  EXPECT_EQ(parts[0].body, content.substr(0, 10));
  EXPECT_EQ(parts[1].body, content.substr(1000, 10));
  EXPECT_EQ(parts[2].body, content.substr(100000, 10));
}
