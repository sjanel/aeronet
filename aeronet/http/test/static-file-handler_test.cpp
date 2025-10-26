#include "aeronet/static-file-handler.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/temp-file.hpp"
#include "connection-state.hpp"
#include "file.hpp"
#include "raw-chars.hpp"

namespace aeronet {

class StaticFileHandlerTest : public ::testing::Test {
 private:
  ConnectionState cs;
  RawChars tmpBuffer;

 protected:
  HttpRequest req;

  test::ScopedTempDir tmpDir;

  void buildReq(std::string_view filePath) {
    std::string raw;
    raw.append("GET ").append("/").append(filePath).append(" HTTP/1.1\r\nHost: h\r\n\r\n");
    cs.inBuffer.assign(raw.data(), raw.size());
  }

  void buildReqWithHeaders(std::string_view filePath, std::string_view extraHeaders) {
    std::string raw;
    raw.append("GET ").append("/").append(filePath).append(" HTTP/1.1\r\nHost: h\r\n");
    if (!extraHeaders.empty()) {
      raw.append(extraHeaders).append("\r\n");
    }
    raw.append("\r\n");
    cs.inBuffer.assign(raw.data(), raw.size());
  }

  void buildReqWithMethod(std::string_view method, std::string_view filePath, std::string_view extraHeaders = "") {
    std::string raw;
    raw.append(method).append(" ").append("/").append(filePath).append(" HTTP/1.1\r\nHost: h\r\n");
    if (!extraHeaders.empty()) {
      raw.append(extraHeaders).append("\r\n");
    }
    raw.append("\r\n");
    cs.inBuffer.assign(raw.data(), raw.size());
  }

  auto setHead() { return req.setHead(cs, tmpBuffer, 4096UL, true); }
};

TEST_F(StaticFileHandlerTest, Basic) {
  using namespace aeronet::test;
  // Create a temp dir and a file inside it

  std::string fileContent = "Hello, static file!";

  ScopedTempFile tmpFile(tmpDir, "hello.txt", fileContent);

  // Construct handler rooted at the temp directory
  StaticFileHandler handler(tmpDir.dirPath());

  // Build a raw HTTP GET head buffer and populate HttpRequest via setHead (friend access)

  buildReq("hello.txt");

  ASSERT_EQ(setHead(), http::StatusCodeOK);

  // Call handler directly and inspect the HttpResponse
  HttpResponse resp = handler(req);

  EXPECT_EQ(resp.statusCode(), http::StatusCodeOK);
  EXPECT_EQ(resp.body(), "");  // Body is not set directly when using file responses

  EXPECT_EQ(resp.headerValueOrEmpty(http::AcceptRanges), "bytes");
  // Other expected headers to be checked

  const File* pFile = resp.file();
  ASSERT_NE(pFile, nullptr);
  EXPECT_EQ(pFile->size(), fileContent.size());
  EXPECT_EQ(pFile->loadAllContent(), fileContent);

  // We expect the handler to set Content-Type (default) and Accept-Ranges
  EXPECT_FALSE(tmpFile.filePath().empty());
}

TEST_F(StaticFileHandlerTest, HeadRequests) {
  using namespace aeronet::test;
  std::string fileContent = "Hello, static file!";
  ScopedTempFile tmpFile(tmpDir, "hello.txt", fileContent);

  StaticFileHandler handler(tmpDir.dirPath());

  // HEAD request
  buildReqWithMethod("HEAD", tmpFile.filename());
  ASSERT_EQ(setHead(), http::StatusCodeOK);

  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.statusCode(), http::StatusCodeOK);
  EXPECT_EQ(resp.body(), "");
  const File* pFile = resp.file();
  ASSERT_NE(pFile, nullptr);
  EXPECT_EQ(pFile->size(), fileContent.size());
}

TEST_F(StaticFileHandlerTest, MethodNotAllowed) {
  using namespace aeronet::test;
  std::string fileContent = "x";
  ScopedTempFile tmpFile(tmpDir, "f.txt", fileContent);

  StaticFileHandler handler(tmpDir.dirPath());

  buildReqWithMethod("POST", tmpFile.filename());
  ASSERT_EQ(setHead(), http::StatusCodeOK);

  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.statusCode(), http::StatusCodeMethodNotAllowed);
  EXPECT_EQ(resp.headerValueOrEmpty(http::Allow), "GET, HEAD");
}

TEST_F(StaticFileHandlerTest, NotFound) {
  StaticFileHandler handler(tmpDir.dirPath());
  buildReq("no-such-file.txt");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.statusCode(), http::StatusCodeNotFound);
}

TEST_F(StaticFileHandlerTest, DirectoryIndexMissing) {
  // Directory exists but no index file -> NotFound
  const auto dirPath = tmpDir.dirPath() / "subdir";
  std::filesystem::create_directory(dirPath);

  StaticFileHandler handler(tmpDir.dirPath());
  buildReq("subdir/index.html");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.statusCode(), http::StatusCodeNotFound);
}

TEST_F(StaticFileHandlerTest, DirectoryIndexPresent) {
  using namespace aeronet::test;
  const auto dirPath = tmpDir.dirPath() / "subdir";
  std::filesystem::create_directories(dirPath);
  ScopedTempFile indexFile(tmpDir, "subdir/index.html", "INDEX");
  StaticFileHandler handler(tmpDir.dirPath());
  buildReq("subdir/index.html");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.statusCode(), http::StatusCodeOK);
}

TEST_F(StaticFileHandlerTest, RangeValid) {
  using namespace aeronet::test;
  std::string fileContent = "0123456789";  // size 10
  ScopedTempFile tmpFile(tmpDir, "num.txt", fileContent);
  StaticFileHandler handler(tmpDir.dirPath());

  buildReqWithHeaders(tmpFile.filename(), "Range: bytes=2-5");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.statusCode(), http::StatusCodePartialContent);
  EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentRange).find("bytes 2-5/") == 0);
}

TEST_F(StaticFileHandlerTest, RangeUnsatisfiable) {
  using namespace aeronet::test;
  std::string fileContent = "0123456789";  // size 10
  ScopedTempFile tmpFile(tmpDir, "num2.txt", fileContent);
  StaticFileHandler handler(tmpDir.dirPath());

  buildReqWithHeaders(tmpFile.filename(), "Range: bytes=100-200");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.statusCode(), http::StatusCodeRangeNotSatisfiable);
  EXPECT_NE(resp.headerValueOrEmpty(http::ContentRange).size(), 0U);
}

TEST_F(StaticFileHandlerTest, ConditionalIfNoneMatchNotModified) {
  using namespace aeronet::test;
  std::string fileContent = "Hello";
  ScopedTempFile tmpFile(tmpDir, "c.txt", fileContent);
  StaticFileHandler handler(tmpDir.dirPath());

  // If-None-Match: * should match any etag and produce 304 when conditional handling is enabled
  buildReqWithHeaders(tmpFile.filename(), "If-None-Match: *");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.statusCode(), http::StatusCodeNotModified);
}

TEST_F(StaticFileHandlerTest, ConditionalIfMatchPreconditionFailed) {
  using namespace aeronet::test;
  std::string fileContent = "Hello";
  ScopedTempFile tmpFile(tmpDir, "c2.txt", fileContent);
  StaticFileHandler handler(tmpDir.dirPath());

  // If-Match with non-matching token should trigger 412
  buildReqWithHeaders(tmpFile.filename(), "If-Match: \"nope\"");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.statusCode(), http::StatusCodePreconditionFailed);
}

}  // namespace aeronet