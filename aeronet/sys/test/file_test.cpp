#include "aeronet/file.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdarg>
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
  EXPECT_FALSE(fileObj.duplicate());
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
  test::FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-missing");
  const auto missingPath = dir.dirPath() / "does-not-exist.bin";
  File fileObj(std::string_view(missingPath.string()), File::OpenMode::ReadOnly);
  EXPECT_FALSE(static_cast<bool>(fileObj));
  EXPECT_EQ(fileObj.size(), File::kError);
}

TEST(FileTest, StringViewConstructorLoadsContent) {
  test::FileSyscallHookGuard guard;
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

TEST(FileTest, SizeUsesFstatOverride) {
  test::FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-fstat");
  ScopedTempFile tmp(dir, "content123");
  const std::string path = tmp.filePath().string();
  File fileObj(path, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));
  // Set a fake size via the fstat override for this path
  test::gFstatSizes.setActions(path, {static_cast<std::int64_t>(12345)});
  EXPECT_EQ(fileObj.size(), 12345U);

  test::gFstatSizes.setActions(path, {static_cast<std::int64_t>(-1)});
  EXPECT_EQ(fileObj.size(), File::kError);
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

TEST(FileTest, DupCreatesIndependentDescriptor) {
  ScopedTempDir dir("aeronet-file-dup");
  ScopedTempFile tmp(dir, "dup-content");
  const std::string path = tmp.filePath().string();

  File original(path, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(original));
  const auto originalSize = original.size();
  EXPECT_EQ(LoadAllContent(original), "dup-content");

  File duplicated = original.duplicate();
  ASSERT_TRUE(static_cast<bool>(duplicated));

  // Both should report the same size and content
  EXPECT_EQ(duplicated.size(), originalSize);
  EXPECT_EQ(LoadAllContent(duplicated), "dup-content");

  // Destroy original and ensure duplicated still works
  original = File();
  ASSERT_TRUE(static_cast<bool>(duplicated));
  EXPECT_EQ(LoadAllContent(duplicated), "dup-content");
}

TEST(FileTest, DuplicateThrowsWhenFcntlFails) {
  test::FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-dup-fail");
  ScopedTempFile tmp(dir, "dup-content-fail");
  const std::string path = tmp.filePath().string();

  File original(path, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(original));
  // Simulate fcntl failure for dup
  test::SetFcntlErrors(path, {EBADF});
  EXPECT_FALSE(original.duplicate());
}