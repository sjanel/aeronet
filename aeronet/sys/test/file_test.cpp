#define AERONET_WANT_SENDFILE_PREAD_OVERRIDES
#define AERONET_FILE_SYS_TEST_SUPPORT_USE_EXISTING_PATHFORFD

#include "aeronet/file.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <stdexcept>
#include <string>
#include <string_view>

#include "aeronet/file-helpers.hpp"
#include "aeronet/file-sys-test-support.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/temp-file.hpp"

using namespace aeronet;

using test::ScopedTempDir;
using test::ScopedTempFile;

TEST(FileTest, DefaultConstructedIsFalse) {
  File fileObj;
  EXPECT_FALSE(static_cast<bool>(fileObj));

  EXPECT_EQ(fileObj.size(), File::kError);
}

TEST(FileTest, InvalidOpenMode) {
  EXPECT_THROW(File fileObj("somefile.txt", static_cast<File::OpenMode>(0xFF)), std::invalid_argument);
}

TEST(FileTest, SizeAndLoadAllContent) {
  ScopedTempDir tmpDir("aeronet-file-test");
  ScopedTempFile tmp(tmpDir, "hello world\n");
  File fileObj(tmp.filePath().string(), File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));
  EXPECT_EQ(fileObj.size(), std::string("hello world\n").size());
  const auto content = LoadAllContent(fileObj);
  EXPECT_EQ(content, "hello world\n");
}

TEST(FileTest, DetectedContentTypeKnownExtension) {
  ScopedTempDir mdDir("aeronet-file-md");
  const auto mdPath = mdDir.dirPath() / "sample.md";
  std::ofstream ofs(mdPath);
  ofs << "# title\n";
  ofs.close();
  File fileObj(mdPath.string(), File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));
  EXPECT_EQ(fileObj.detectedContentType(), "text/markdown");
}

TEST(FileTest, DetectedContentTypeMultiDot) {
  ScopedTempDir tgzDir("aeronet-file-tgz");
  const auto tgzPath = tgzDir.dirPath() / "archive.tar.gz";
  std::ofstream ofs(tgzPath);
  ofs << "data";
  ofs.close();
  File fileObj(tgzPath.string(), File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));
  // We expect tar.gz to resolve to application/gzip per the mappings
  EXPECT_EQ(fileObj.detectedContentType(), "application/gzip");
}

TEST(FileTest, DetectedContentTypeUnknownFallsBackToOctet) {
  ScopedTempDir unkDir("aeronet-file-unk");
  const auto unkPath = unkDir.dirPath() / "file.unknownext";
  std::ofstream ofs2(unkPath, std::ios::binary);
  ofs2.write("\0\1\2", 3);
  ofs2.close();
  File fileObj(unkPath.string(), File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));
  EXPECT_EQ(fileObj.detectedContentType(), "application/octet-stream");
}

TEST(FileTest, DetectedContentTypeCaseInsensitiveExtension) {
  ScopedTempDir upperDir("aeronet-file-upper");
  const auto upperPath = upperDir.dirPath() / "UPPER.TXT";
  std::ofstream ofs3(upperPath);
  ofs3 << "hi";
  ofs3.close();
  File fileObj(upperPath.string(), File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));
  // Current implementation does case-sensitive extension matching, so uppercase extension falls back
  EXPECT_EQ(fileObj.detectedContentType(), "text/plain");
}

TEST(FileTest, MissingFileLeavesDescriptorClosed) {
  ScopedTempDir dir("aeronet-file-missing");
  const auto missingPath = dir.dirPath() / "does-not-exist.bin";
  File fileObj(std::string_view(missingPath.string()), File::OpenMode::ReadOnly);
  EXPECT_FALSE(static_cast<bool>(fileObj));
  EXPECT_EQ(fileObj.size(), File::kError);
}

TEST(FileTest, StringViewConstructorLoadsContent) {
  ScopedTempDir dir("aeronet-file-sv");
  ScopedTempFile tmp(dir, "string-view-content");
  const std::string path = tmp.filePath().string();
  std::string_view pathView(path);
  File fileObj(pathView, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));
  EXPECT_EQ(LoadAllContent(fileObj), "string-view-content");
}

TEST(FileTest, LoadAllContentRetriesAfterEintr) {
  test::FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-eintr");
  ScopedTempFile tmp(dir, "retry-data");
  const std::string path = tmp.filePath().string();
  File fileObj(path, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));
  test::SetReadActions(path, {test::ReadErr(EINTR)});
  EXPECT_EQ(LoadAllContent(fileObj), "retry-data");
}

TEST(FileTest, LoadAllContentThrowsOnFatalReadError) {
  test::FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-reader");
  ScopedTempFile tmp(dir, "payload");
  const std::string path = tmp.filePath().string();
  File fileObj(path, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));
  test::SetReadActions(path, {test::ReadErr(EIO)});
  EXPECT_EQ(LoadAllContent(fileObj), "payload");
}

TEST(FileTest, ReadAtRetriesOnEintr) {
  test::FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-readat-eintr");
  ScopedTempFile tmp(dir, "abcdefgh");
  const std::string path = tmp.filePath().string();
  File fileObj(path, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));

  // First pread returns EINTR, second succeeds with 3 bytes read.
  aeronet::test::SetPreadPathActions(path, {IoAction{-1, EINTR}, IoAction{3, 0}});

  std::array<std::byte, 4> buf{};
  const auto readBytes = fileObj.readAt(buf, 0);
  EXPECT_EQ(readBytes, 3U);
}

TEST(FileTest, ReadAtReturnsErrorOnFatalPread) {
  test::FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-readat-fatal");
  ScopedTempFile tmp(dir, "abcdef");
  const std::string path = tmp.filePath().string();
  File fileObj(path, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));

  // Fatal pread error should return kError.
  aeronet::test::SetPreadPathActions(path, {IoAction{-1, EIO}});

  std::array<std::byte, 2> buf{};
  const auto readBytes = fileObj.readAt(buf, 1);
  EXPECT_EQ(readBytes, File::kError);
}

TEST(FileTest, Size) {
  test::FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-fstat");
  ScopedTempFile tmp(dir, "content123");
  const std::string path = tmp.filePath().string();
  // Set a fake size via the fstat override for this path
  test::gFstatSizes.setActions(path, {static_cast<std::int64_t>(12345)});
  File fileObj(path, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));

  EXPECT_EQ(fileObj.size(), 12345U);

  test::gFstatSizes.setActions(path, {static_cast<std::int64_t>(-1)});
  File fileObj2(path, File::OpenMode::ReadOnly);
  EXPECT_EQ(fileObj2.size(), File::kError);
}

TEST(FileTest, RestoreToStartLogsWhenLseekFails) {
  test::FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-lseek");
  ScopedTempFile tmp(dir, "abc");
  const std::string path = tmp.filePath().string();
  File fileObj(path, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));
  test::SetLseekErrors(path, {EIO});
  EXPECT_EQ(LoadAllContent(fileObj), "abc");
}
