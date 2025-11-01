#include "aeronet/static-file-handler.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
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
    cs.inBuffer.assign(raw);
  }

  void buildReqWithHeaders(std::string_view filePath, std::string_view extraHeaders) {
    std::string raw;
    raw.append("GET ").append("/").append(filePath).append(" HTTP/1.1\r\nHost: h\r\n");
    if (!extraHeaders.empty()) {
      raw.append(extraHeaders).append("\r\n");
    }
    raw.append("\r\n");
    cs.inBuffer.assign(raw);
  }

  void buildReqWithMethod(std::string_view method, std::string_view filePath, std::string_view extraHeaders = "") {
    std::string raw;
    raw.append(method).append(" ").append("/").append(filePath).append(" HTTP/1.1\r\nHost: h\r\n");
    if (!extraHeaders.empty()) {
      raw.append(extraHeaders).append("\r\n");
    }
    raw.append("\r\n");
    cs.inBuffer.assign(raw);
  }

  auto setHead() { return req.initTrySetHead(cs, tmpBuffer, 4096UL, true, nullptr); }
};

TEST_F(StaticFileHandlerTest, Basic) {
  // Create a temp dir and a file inside it

  std::string fileContent = "Hello, static file!";

  aeronet::test::ScopedTempFile tmpFile(tmpDir, fileContent);

  // Construct handler rooted at the temp directory created by tmpFile
  StaticFileHandler handler(tmpFile.dirPath());

  // Build a raw HTTP GET head buffer and populate HttpRequest via setHead (friend access)

  buildReq(tmpFile.filename());

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
  ScopedTempFile tmpFile(tmpDir, fileContent);

  StaticFileHandler handler(tmpFile.dirPath());

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
  ScopedTempFile tmpFile(tmpDir, fileContent);

  StaticFileHandler handler(tmpFile.dirPath());

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
  // Create index file in the subdirectory manually; ScopedTempFile creates files
  // directly under the provided ScopedTempDir and does not accept nested paths.
  const auto indexPath = dirPath / "index.html";
  {
    std::ofstream ofs(indexPath);
    ofs << "INDEX";
  }
  StaticFileHandler handler(tmpDir.dirPath());
  buildReq("subdir/index.html");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.statusCode(), http::StatusCodeOK);
}

TEST_F(StaticFileHandlerTest, DirectoryListingEnabled) {
  const auto dirPath = tmpDir.dirPath() / "assets";
  std::filesystem::create_directories(dirPath);
  {
    std::ofstream(dirPath / "a.txt") << "a";
    std::ofstream(dirPath / "b.txt") << "b";
  }
  std::filesystem::create_directory(dirPath / "nested");

  StaticFileConfig cfg;
  cfg.enableDirectoryIndex = true;
  cfg.withDefaultIndex("");
  StaticFileHandler handler(tmpDir.dirPath(), std::move(cfg));

  buildReq("assets/");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);

  EXPECT_EQ(resp.statusCode(), http::StatusCodeOK);
  EXPECT_EQ(resp.headerValueOrEmpty("Cache-Control"), "no-cache");
  const std::string_view body = resp.body();
  EXPECT_TRUE(body.contains("Index of /assets/"));
  EXPECT_TRUE(body.contains("a.txt"));
  // The displayed name should not contain the literal '/', CSS adds it via a.dir::after.
  EXPECT_TRUE(body.contains("nested"));
  // But the href for directories should include the trailing slash. Find the link to "nested/".
  ASSERT_TRUE(body.contains("href=\"nested/\"")) << "Directory listing body:\n" << body;
}

TEST_F(StaticFileHandlerTest, DirectoryListingRedirectsWithoutSlash) {
  const auto dirPath = tmpDir.dirPath() / "assets";
  std::filesystem::create_directory(dirPath);

  StaticFileConfig cfg;
  cfg.enableDirectoryIndex = true;
  cfg.withDefaultIndex("");
  StaticFileHandler handler(tmpDir.dirPath(), std::move(cfg));

  buildReq("assets");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);

  EXPECT_EQ(resp.statusCode(), http::StatusCodeMovedPermanently);
  EXPECT_EQ(resp.headerValueOrEmpty(http::Location), "/assets/");
}

TEST_F(StaticFileHandlerTest, DirectoryListingHonorsHiddenFilesFlag) {
  const auto dirPath = tmpDir.dirPath() / "assets";
  std::filesystem::create_directories(dirPath);
  std::ofstream(dirPath / ".secret") << "hidden";
  std::ofstream(dirPath / "visible.txt") << "content";

  StaticFileConfig cfgNoHidden;
  cfgNoHidden.enableDirectoryIndex = true;
  cfgNoHidden.withDefaultIndex("");
  StaticFileHandler handlerNoHidden(tmpDir.dirPath(), cfgNoHidden);

  buildReq("assets/");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse respHidden = handlerNoHidden(req);
  const std::string_view bodyHidden = respHidden.body();
  EXPECT_EQ(respHidden.statusCode(), http::StatusCodeOK);
  EXPECT_FALSE(bodyHidden.contains(".secret"));
  EXPECT_TRUE(bodyHidden.contains("visible.txt"));

  StaticFileConfig cfgShowHidden;
  cfgShowHidden.enableDirectoryIndex = true;
  cfgShowHidden.showHiddenFiles = true;
  cfgShowHidden.withDefaultIndex("");
  StaticFileHandler handlerShowHidden(tmpDir.dirPath(), cfgShowHidden);

  buildReq("assets/");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse respShow = handlerShowHidden(req);
  const std::string_view bodyShow = respShow.body();
  EXPECT_EQ(respShow.statusCode(), http::StatusCodeOK);
  EXPECT_TRUE(bodyShow.contains(".secret"));
}

TEST_F(StaticFileHandlerTest, RangeValid) {
  using namespace aeronet::test;
  std::string fileContent = "0123456789";  // size 10
  ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpFile.dirPath());

  buildReqWithHeaders(tmpFile.filename(), "Range: bytes=2-5");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.statusCode(), http::StatusCodePartialContent);
  EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentRange).starts_with("bytes 2-5/"));
}

TEST_F(StaticFileHandlerTest, RangeUnsatisfiable) {
  using namespace aeronet::test;
  std::string fileContent = "0123456789";  // size 10
  ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpFile.dirPath());

  buildReqWithHeaders(tmpFile.filename(), "Range: bytes=100-200");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.statusCode(), http::StatusCodeRangeNotSatisfiable);
  EXPECT_NE(resp.headerValueOrEmpty(http::ContentRange).size(), 0U);
}

TEST_F(StaticFileHandlerTest, ConditionalIfNoneMatchNotModified) {
  using namespace aeronet::test;
  std::string fileContent = "Hello";
  ScopedTempFile tmpFile(tmpDir, fileContent);
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
  ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpDir.dirPath());

  // If-Match with non-matching token should trigger 412
  buildReqWithHeaders(tmpFile.filename(), "If-Match: \"nope\"");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.statusCode(), http::StatusCodePreconditionFailed);
}

}  // namespace aeronet