#include "aeronet/static-file-handler.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "aeronet/connection-state.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/static-file-config.hpp"
#include "aeronet/temp-file.hpp"

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

  static void writeFileWithSize(const std::filesystem::path& path, std::size_t size, char fill = 'x') {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    std::string chunk(1024, fill);
    std::size_t remaining = size;
    while (remaining > 0) {
      const auto writeLen = std::min<std::size_t>(remaining, chunk.size());
      ofs.write(chunk.data(), static_cast<std::streamsize>(writeLen));
      remaining -= writeLen;
    }
  }
};

TEST(StaticFileHandlerConstructorTest, ThrowsWhenRootMissing) {
  const auto bogusRoot = std::filesystem::path("/tmp/aeronet-no-such-dir");
  EXPECT_THROW({ StaticFileHandler handler(bogusRoot); }, std::invalid_argument);
}

TEST_F(StaticFileHandlerTest, ConstructorFallsBackToAbsoluteOnCanonicalFailure) {
  const auto blocked = tmpDir.dirPath() / "blocked";
  std::filesystem::create_directory(blocked);
  std::filesystem::permissions(blocked, std::filesystem::perms::none, std::filesystem::perm_options::replace);
  EXPECT_NO_THROW({ StaticFileHandler handler(blocked); });
  std::filesystem::permissions(blocked, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
}

TEST_F(StaticFileHandlerTest, Basic) {
  // Create a temp dir and a file inside it

  std::string fileContent = "Hello, static file!";

  test::ScopedTempFile tmpFile(tmpDir, fileContent);

  // Construct handler rooted at the temp directory created by tmpFile
  StaticFileHandler handler(tmpFile.dirPath());

  // Build a raw HTTP GET head buffer and populate HttpRequest via setHead (friend access)

  buildReq(tmpFile.filename());

  ASSERT_EQ(setHead(), http::StatusCodeOK);

  // Call handler directly and inspect the HttpResponse
  HttpResponse resp = handler(req);

  EXPECT_EQ(resp.status(), http::StatusCodeOK);
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
  std::string fileContent = "Hello, static file!";
  test::ScopedTempFile tmpFile(tmpDir, fileContent);

  StaticFileHandler handler(tmpFile.dirPath());

  // HEAD request
  buildReqWithMethod("HEAD", tmpFile.filename());
  ASSERT_EQ(setHead(), http::StatusCodeOK);

  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.body(), "");
  const File* pFile = resp.file();
  ASSERT_NE(pFile, nullptr);
  EXPECT_EQ(pFile->size(), fileContent.size());
}

TEST_F(StaticFileHandlerTest, MethodNotAllowed) {
  std::string fileContent = "x";
  test::ScopedTempFile tmpFile(tmpDir, fileContent);

  StaticFileHandler handler(tmpFile.dirPath());

  buildReqWithMethod("POST", tmpFile.filename());
  ASSERT_EQ(setHead(), http::StatusCodeOK);

  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodeMethodNotAllowed);
  EXPECT_EQ(resp.headerValueOrEmpty(http::Allow), "GET, HEAD");
}

TEST_F(StaticFileHandlerTest, NotFound) {
  StaticFileHandler handler(tmpDir.dirPath());
  buildReq("no-such-file.txt");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodeNotFound);
}

TEST_F(StaticFileHandlerTest, DefaultIndexServedWhenDirectoryRequested) {
  const auto dirPath = tmpDir.dirPath() / "pages";
  std::filesystem::create_directories(dirPath);
  const std::string fileContent = "Welcome";
  {
    std::ofstream ofs(dirPath / "index.html");
    ofs << fileContent;
  }

  StaticFileHandler handler(tmpDir.dirPath());
  buildReq("pages/");
  ASSERT_EQ(setHead(), http::StatusCodeOK);

  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  const File* servedFile = resp.file();
  ASSERT_NE(servedFile, nullptr);
  EXPECT_EQ(servedFile->size(), fileContent.size());
}

TEST_F(StaticFileHandlerTest, DirectoryIndexDisabledReturnsNotFound) {
  const auto dirPath = tmpDir.dirPath() / "assets";
  std::filesystem::create_directory(dirPath);

  StaticFileHandler handler(tmpDir.dirPath());
  buildReq("assets/");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodeNotFound);
}

TEST_F(StaticFileHandlerTest, RejectsTraversalSegments) {
  StaticFileHandler handler(tmpDir.dirPath());
  buildReq("../secret");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodeNotFound);
}

TEST_F(StaticFileHandlerTest, EmptyRequestPathDefaultsToRoot) {
  StaticFileHandler handler(tmpDir.dirPath());
  buildReq("");  // results in "/" request
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodeNotFound);
}

TEST_F(StaticFileHandlerTest, DirectoryIndexMissing) {
  // Directory exists but no index file -> NotFound
  const auto dirPath = tmpDir.dirPath() / "subdir";
  std::filesystem::create_directory(dirPath);

  StaticFileHandler handler(tmpDir.dirPath());
  buildReq("subdir/index.html");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodeNotFound);
}

TEST_F(StaticFileHandlerTest, DirectoryIndexPresent) {
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
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
}

TEST_F(StaticFileHandlerTest, DirectoryListingEscapesAndFormatsSizes) {
  const auto dirPath = tmpDir.dirPath() / "assets";
  std::filesystem::create_directories(dirPath);

  writeFileWithSize(dirPath / "a-mix&<>\"'name.txt", 1536);
  writeFileWithSize(dirPath / "b-rounding.bin", 10189);
  const auto danglingTarget = dirPath / "does-not-exist.txt";
  const auto danglingLink = dirPath / "c-dangling";
  std::error_code ec;
  std::filesystem::create_symlink(danglingTarget, danglingLink, ec);
  ASSERT_FALSE(ec) << ec.message();
  writeFileWithSize(dirPath / "d-large.bin", 25000);

  StaticFileConfig cfg;
  cfg.enableDirectoryIndex = true;
  cfg.showHiddenFiles = true;
  cfg.withDirectoryListingCss("body{color:red;}");

  for (std::size_t maxEntriesToList = 0U; maxEntriesToList <= 5U; ++maxEntriesToList) {
    cfg.maxEntriesToList = maxEntriesToList;
    StaticFileHandler handler(tmpDir.dirPath(), cfg);

    buildReq("assets/");
    ASSERT_EQ(setHead(), http::StatusCodeOK);

    HttpResponse resp = handler(req);
    ASSERT_EQ(resp.status(), http::StatusCodeOK);

    const std::string_view body = resp.body();
    if (maxEntriesToList < 4U) {
      EXPECT_EQ(resp.headerValueOrEmpty("X-Directory-Listing-Truncated"), "1");
      EXPECT_TRUE(body.contains(std::format("Listing truncated after {} entries.", maxEntriesToList)));
    } else {
      EXPECT_EQ(resp.headerValueOrEmpty("X-Directory-Listing-Truncated"), "0");
      EXPECT_FALSE(body.contains("Listing truncated after"));
    }
    EXPECT_TRUE(body.contains("<td class=\"modified\">-</td>"));
    EXPECT_TRUE(body.contains("body{color:red;}"));
    EXPECT_EQ(body.contains("a-mix&amp;&lt;&gt;&quot;&#39;name.txt"), maxEntriesToList >= 1U);
    EXPECT_EQ(body.contains("1.5 KB"), maxEntriesToList >= 1U);
    EXPECT_EQ(body.contains("b-rounding.bin"), maxEntriesToList >= 2U);
    EXPECT_EQ(body.contains("10 KB"), maxEntriesToList >= 2U);
    EXPECT_EQ(body.contains("c-dangling"), maxEntriesToList >= 3U);
    EXPECT_EQ(body.contains("d-large.bin"), maxEntriesToList >= 4U);
    EXPECT_EQ(body.contains("24 KB"), maxEntriesToList >= 4U);
  }
}

TEST_F(StaticFileHandlerTest, DirectoryListingFormatsLargeSizesWithoutDecimals) {
  const auto dirPath = tmpDir.dirPath() / "assets";
  std::filesystem::create_directories(dirPath);
  writeFileWithSize(dirPath / "large.bin", 25000);

  StaticFileConfig cfg;
  cfg.enableDirectoryIndex = true;
  StaticFileHandler handler(tmpDir.dirPath(), cfg);

  buildReq("assets/");
  ASSERT_EQ(setHead(), http::StatusCodeOK);

  HttpResponse resp = handler(req);
  ASSERT_EQ(resp.status(), http::StatusCodeOK);
  const std::string_view body = resp.body();
  EXPECT_NE(body.find("24 KB"), std::string_view::npos);
  EXPECT_EQ(resp.headerValueOrEmpty("X-Directory-Listing-Truncated"), "0");
}

TEST_F(StaticFileHandlerTest, DirectoryListingUsesCustomRenderer) {
  const auto dirPath = tmpDir.dirPath() / "assets";
  std::filesystem::create_directories(dirPath);
  writeFileWithSize(dirPath / "alpha.txt", 4);
  writeFileWithSize(dirPath / "beta.txt", 8);

  StaticFileConfig cfg;
  cfg.enableDirectoryIndex = true;
  bool rendererCalled = false;
  cfg.directoryIndexRenderer = [&rendererCalled](const std::filesystem::path& directory,
                                                 std::span<const std::filesystem::directory_entry> entries) {
    rendererCalled = true;
    EXPECT_FALSE(entries.empty());
    EXPECT_TRUE(std::filesystem::exists(directory));
    return std::string{"<html>custom</html>"};
  };
  StaticFileHandler handler(tmpDir.dirPath(), cfg);

  buildReq("assets/");
  ASSERT_EQ(setHead(), http::StatusCodeOK);

  HttpResponse resp = handler(req);
  ASSERT_TRUE(rendererCalled);
  EXPECT_EQ(resp.body(), "<html>custom</html>");
  EXPECT_EQ(resp.headerValueOrEmpty("X-Directory-Listing-Truncated"), "0");
}

TEST_F(StaticFileHandlerTest, DirectoryListingFailsWhenDirectoryUnreadable) {
  const auto dirPath = tmpDir.dirPath() / "sealed";
  std::filesystem::create_directory(dirPath);

  if (geteuid() == 0) {
    GTEST_SKIP() << "Running as root; unreadable-directory semantics unreliable in containers";
  }

  StaticFileConfig cfg;
  cfg.enableDirectoryIndex = true;
  StaticFileHandler handler(tmpDir.dirPath(), cfg);

  buildReq("sealed/");
  ASSERT_EQ(setHead(), http::StatusCodeOK);

  std::filesystem::permissions(dirPath, std::filesystem::perms::none, std::filesystem::perm_options::replace);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodeInternalServerError);
  std::filesystem::permissions(dirPath, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
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

  EXPECT_EQ(resp.status(), http::StatusCodeOK);
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

  EXPECT_EQ(resp.status(), http::StatusCodeMovedPermanently);
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
  EXPECT_EQ(respHidden.status(), http::StatusCodeOK);
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
  EXPECT_EQ(respShow.status(), http::StatusCodeOK);
  EXPECT_TRUE(bodyShow.contains(".secret"));
}

TEST_F(StaticFileHandlerTest, RangeValid) {
  std::string fileContent = "0123456789";  // size 10
  test::ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpFile.dirPath());

  buildReqWithHeaders(tmpFile.filename(), "Range: bytes=2-5");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodePartialContent);
  EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentRange).starts_with("bytes 2-5/"));
}

TEST_F(StaticFileHandlerTest, RangeUnsatisfiable) {
  std::string fileContent = "0123456789";  // size 10
  test::ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpFile.dirPath());

  buildReqWithHeaders(tmpFile.filename(), "Range: bytes=100-200");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodeRangeNotSatisfiable);
  EXPECT_NE(resp.headerValueOrEmpty(http::ContentRange).size(), 0U);
}

TEST_F(StaticFileHandlerTest, RangeSuffixBytesServed) {
  const std::string fileContent = "0123456789";
  test::ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpFile.dirPath());

  buildReqWithHeaders(tmpFile.filename(), "Range: bytes=-3");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodePartialContent);
  EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentRange).starts_with("bytes 7-9/"));
}

TEST_F(StaticFileHandlerTest, RangeOpenEndedServed) {
  const std::string fileContent = "0123456789";
  test::ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpFile.dirPath());

  buildReqWithHeaders(tmpFile.filename(), "Range: bytes=3-");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodePartialContent);
  EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentRange).starts_with("bytes 3-9/"));
}

TEST_F(StaticFileHandlerTest, RangeParserTrimsWhitespace) {
  const std::string fileContent = "0123456789";
  test::ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpFile.dirPath());

  buildReqWithHeaders(tmpFile.filename(), "Range: bytes= 2 - 5 ");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodePartialContent);
  EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentRange).starts_with("bytes 2-5/"));
}

TEST_F(StaticFileHandlerTest, RangeInvalidFormsReturnErrors) {
  const std::string fileContent = "0123456789";
  test::ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpFile.dirPath());

  struct Case {
    const char* header;
    const char* expectedBody;
  };
  const std::vector<Case> cases = {
      {"Range: foo=1-2", "Invalid Range\n"},
      {"Range: bytes=", "Invalid Range\n"},
      {"Range: bytes=5", "Invalid Range\n"},
      {"Range: bytes=-0", "Invalid Range\n"},
      {"Range: bytes=1-2,3-4", "Invalid Range\n"},
      {"Range: bytes=5-a", "Invalid Range\n"},
      {"Range: bytes=15-1", "Range Not Satisfiable\n"},
  };

  for (const auto& testCase : cases) {
    buildReqWithHeaders(tmpFile.filename(), testCase.header);
    ASSERT_EQ(setHead(), http::StatusCodeOK);
    HttpResponse resp = handler(req);
    EXPECT_EQ(resp.status(), http::StatusCodeRangeNotSatisfiable) << testCase.header;
    EXPECT_EQ(resp.body(), testCase.expectedBody) << testCase.header;
  }
}

TEST_F(StaticFileHandlerTest, RangeRequestsOnEmptyFileAreUnsatisfiable) {
  const auto filePath = tmpDir.dirPath() / "empty.bin";
  {
    std::ofstream ofs(filePath);
  }

  StaticFileHandler handler(tmpDir.dirPath());
  buildReqWithHeaders(filePath.filename().string(), "Range: bytes=0-0");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodeRangeNotSatisfiable);
  EXPECT_EQ(resp.body(), "Range Not Satisfiable\n");
}

TEST_F(StaticFileHandlerTest, EmptyRangeHeaderIsIgnored) {
  const std::string fileContent = "0123456789";
  test::ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpFile.dirPath());

  buildReqWithHeaders(tmpFile.filename(), "Range: ");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentRange), "");
}

TEST_F(StaticFileHandlerTest, IfRangeHonorsEtagsAndDates) {
  const std::string fileContent = "0123456789";
  test::ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpDir.dirPath());

  buildReq(tmpFile.filename());
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse baseResp = handler(req);
  const std::string etag(baseResp.headerValueOrEmpty(http::ETag));
  const std::string lastModified(baseResp.headerValueOrEmpty(http::LastModified));
  ASSERT_FALSE(etag.empty());
  ASSERT_FALSE(lastModified.empty());
  const std::string futureDate = "Thu, 31 Dec 2099 23:59:59 GMT";

  auto makeRangeHeaders = [&](std::string_view rangeHeader, std::string_view ifRangeHeader) {
    std::string headers(rangeHeader);
    if (!ifRangeHeader.empty()) {
      headers.append("\r\nIf-Range: ").append(ifRangeHeader);
    }
    return headers;
  };

  auto expectStatus = [&](std::string_view headers, http::StatusCode expected) {
    buildReqWithHeaders(tmpFile.filename(), headers);
    ASSERT_EQ(setHead(), http::StatusCodeOK);
    HttpResponse resp = handler(req);
    EXPECT_EQ(resp.status(), expected) << headers;
  };

  expectStatus(makeRangeHeaders("Range: bytes=0-1", etag), http::StatusCodePartialContent);
  expectStatus(makeRangeHeaders("Range: bytes=0-1", etag + "-mismatch"), http::StatusCodeOK);
  expectStatus(makeRangeHeaders("Range: bytes=0-1", "W/\"weak\""), http::StatusCodeOK);
  expectStatus(makeRangeHeaders("Range: bytes=0-1", "   "), http::StatusCodeOK);
  expectStatus(makeRangeHeaders("Range: bytes=0-1", futureDate), http::StatusCodePartialContent);
  expectStatus(makeRangeHeaders("Range: bytes=0-1", "Sat, 01 Jan 2000 00:00:00 GMT"), http::StatusCodeOK);
  expectStatus(makeRangeHeaders("Range: bytes=0-1", "INVALID"), http::StatusCodeOK);
}

TEST_F(StaticFileHandlerTest, ContentTypeResolverOverridesDefault) {
  const auto filePath = tmpDir.dirPath() / "resolver.txt";
  {
    std::ofstream ofs(filePath);
    ofs << "hello";
  }

  StaticFileConfig cfg;
  cfg.contentTypeResolver = [](std::string_view path) {
    EXPECT_TRUE(path.ends_with("resolver.txt"));
    return std::string{"text/x-special"};
  };
  StaticFileHandler handler(tmpDir.dirPath(), cfg);

  buildReq(filePath.filename().string());
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/x-special");
}

TEST_F(StaticFileHandlerTest, UsesDefaultContentTypeWhenResolverEmpty) {
  const auto filePath = tmpDir.dirPath() / "default.bin";
  {
    std::ofstream ofs(filePath);
    ofs << "hello";
  }

  StaticFileConfig cfg;
  cfg.withDefaultContentType("application/x-default");
  StaticFileHandler handler(tmpDir.dirPath(), cfg);

  buildReq(filePath.filename().string());
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "application/x-default");
}

TEST_F(StaticFileHandlerTest, ConditionalIfNoneMatchNotModified) {
  std::string fileContent = "Hello";
  test::ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpDir.dirPath());

  // If-None-Match: * should match any etag and produce 304 when conditional handling is enabled
  buildReqWithHeaders(tmpFile.filename(), "If-None-Match: *");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodeNotModified);
}

TEST_F(StaticFileHandlerTest, ConditionalIfMatchPreconditionFailed) {
  std::string fileContent = "Hello";
  test::ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpDir.dirPath());

  // If-Match with non-matching token should trigger 412
  buildReqWithHeaders(tmpFile.filename(), "If-Match: \"nope\"");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodePreconditionFailed);
}

TEST_F(StaticFileHandlerTest, ConditionalIfMatchRejectsWeakValidators) {
  std::string fileContent = "Hello";
  test::ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpDir.dirPath());

  buildReqWithHeaders(tmpFile.filename(), "If-Match: W/\"etag\"");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodePreconditionFailed);
}

TEST_F(StaticFileHandlerTest, ConditionalIfUnmodifiedSinceFailsWhenOutdated) {
  std::string fileContent = "Hello";
  test::ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpDir.dirPath());

  buildReqWithHeaders(tmpFile.filename(), "If-Unmodified-Since: Sat, 01 Jan 2000 00:00:00 GMT");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodePreconditionFailed);
  EXPECT_EQ(resp.body(), "Precondition Failed\n");
}

TEST_F(StaticFileHandlerTest, ConditionalIfModifiedSinceReturnsNotModified) {
  std::string fileContent = "Hello";
  test::ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpDir.dirPath());

  buildReqWithHeaders(tmpFile.filename(), "If-Modified-Since: Thu, 31 Dec 2099 23:59:59 GMT");
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodeNotModified);
  EXPECT_EQ(resp.body(), "");
}

TEST_F(StaticFileHandlerTest, ConditionalIfNoneMatchParsesTokenLists) {
  std::string fileContent = "Hello";
  test::ScopedTempFile tmpFile(tmpDir, fileContent);
  StaticFileHandler handler(tmpDir.dirPath());

  buildReq(tmpFile.filename());
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse firstResp = handler(req);
  const std::string etag(firstResp.headerValueOrEmpty(http::ETag));
  ASSERT_FALSE(etag.empty());

  std::string header = "If-None-Match: \"bogus\", ";
  header.append(etag);
  buildReqWithHeaders(tmpFile.filename(), header);
  ASSERT_EQ(setHead(), http::StatusCodeOK);
  HttpResponse resp = handler(req);
  EXPECT_EQ(resp.status(), http::StatusCodeNotModified);
}

}  // namespace aeronet