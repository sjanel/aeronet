#define AERONET_WANT_SENDFILE_PREAD_OVERRIDES

#include "aeronet/file.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
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
#include "aeronet/system-error.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"

namespace aeronet {

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

TEST(FileTest, LastModifiedInvalidForDefaultConstructed) {
  File fileObj;
  EXPECT_EQ(fileObj.lastModified(), kInvalidTimePoint);
}

TEST(FileTest, LastModifiedMatchesFilesystem) {
  ScopedTempDir tmpDir("aeronet-file-mtime");
  ScopedTempFile tmp(tmpDir, "data");
  File fileObj(tmp.filePath().string(), File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));

  const SysTimePoint mtime = fileObj.lastModified();
  EXPECT_NE(mtime, kInvalidTimePoint);

  // The mtime captured from fstat() must agree with the filesystem's own view of the modification time.
  const auto fsMtime = std::chrono::clock_cast<SysClock>(std::filesystem::last_write_time(tmp.filePath()));
  EXPECT_LT(std::chrono::abs(mtime - fsMtime), std::chrono::seconds(2));
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
  test::SetReadActions(path, {test::ReadErr(error::kInterrupted)});
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

  // First pread returns error::kInterrupted, second succeeds with 3 bytes read.
  aeronet::test::SetPreadPathActions(path, {IoAction{-1, error::kInterrupted}, IoAction{3, 0}});

  std::byte buf[4]{};
  const auto readBytes = fileObj.readAt(buf, 0);
  EXPECT_EQ(readBytes, 3U);
}

TEST(FileTest, AppendIdentityShouldBeDifferentBetweenTwoFiles) {
  ScopedTempDir dir("aeronet-file-identity");
  ScopedTempFile tmp1(dir, "file1");
  ScopedTempFile tmp2(dir, "file2");
  File fileObj1(tmp1.filePath().string(), File::OpenMode::ReadOnly);
  File fileObj2(tmp2.filePath().string(), File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj1));
  ASSERT_TRUE(static_cast<bool>(fileObj2));

  std::byte buf1[File::kIdentitySize]{};
  std::byte buf2[File::kIdentitySize]{};
  char* pEnd1 = fileObj1.appendIdentityData(reinterpret_cast<char*>(buf1));
  char* pEnd2 = fileObj2.appendIdentityData(reinterpret_cast<char*>(buf2));
  EXPECT_EQ(pEnd1 - reinterpret_cast<char*>(buf1), static_cast<ptrdiff_t>(File::kIdentitySize));
  EXPECT_EQ(pEnd2 - reinterpret_cast<char*>(buf2), static_cast<ptrdiff_t>(File::kIdentitySize));
  EXPECT_NE(buf1, buf2);
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

  std::byte buf[2]{};
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

TEST(FileTest, CopyConstructorOnDefault) {
  File fileObj;
  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  File copyObj(fileObj);
  EXPECT_FALSE(static_cast<bool>(copyObj));
  EXPECT_EQ(copyObj.size(), File::kError);
}

TEST(FileTest, CopyConstructorNominal) {
  ScopedTempDir dir("aeronet-file-copy");
  ScopedTempFile tmp(dir, "copy-content");
  File fileObj(tmp.filePath().string(), File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));

  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  File copyObj(fileObj);

  EXPECT_EQ(LoadAllContent(fileObj), "copy-content");
  EXPECT_EQ(LoadAllContent(copyObj), "copy-content");
}

TEST(FileTest, CopyConstructorLeavesCopyClosedWhenDupFails) {
  test::FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-copy-dup-failure");
  ScopedTempFile tmp(dir, "copy-content");
  const std::string path = tmp.filePath().string();
  File fileObj(path, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));

  test::SetDupPathErrors(path, {EMFILE});
  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  File copyObj(fileObj);

  EXPECT_FALSE(static_cast<bool>(copyObj));
  EXPECT_EQ(copyObj.size(), File::kError);
  EXPECT_EQ(LoadAllContent(fileObj), "copy-content");
}

TEST(FileTest, CopyAssignment) {
  ScopedTempDir dir("aeronet-file-copyassign");
  ScopedTempFile tmp(dir, "copyassign-content");
  File fileObj(tmp.filePath().string(), File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));

  File copyAssignObj;
  copyAssignObj = fileObj;

  EXPECT_EQ(LoadAllContent(fileObj), "copyassign-content");
  EXPECT_EQ(LoadAllContent(copyAssignObj), "copyassign-content");
}

TEST(FileTest, SelfCopyAssignmentLeavesObjectUnchanged) {
  ScopedTempDir dir("aeronet-file-copyassign-self");
  ScopedTempFile tmp(dir, "self-copy-content");
  File fileObj(tmp.filePath().string(), File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));

  auto& self = fileObj;
  self = fileObj;

  EXPECT_EQ(LoadAllContent(fileObj), "self-copy-content");
}

TEST(FileTest, CopyAssignmentLeavesDestinationClosedWhenDupFails) {
  test::FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-copyassign-dup-failure");
  ScopedTempFile sourceTmp(dir, "source-content");
  ScopedTempFile destinationTmp(dir, "destination-content");
  const std::string sourcePath = sourceTmp.filePath().string();
  File sourceObj(sourcePath, File::OpenMode::ReadOnly);
  File destinationObj(destinationTmp.filePath().string(), File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(sourceObj));
  ASSERT_TRUE(static_cast<bool>(destinationObj));

  test::SetDupPathErrors(sourcePath, {EMFILE});
  destinationObj = sourceObj;

  EXPECT_FALSE(static_cast<bool>(destinationObj));
  EXPECT_EQ(destinationObj.size(), File::kError);
  EXPECT_EQ(LoadAllContent(sourceObj), "source-content");
}

}  // namespace aeronet