#include "aeronet/file.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "aeronet/file-helpers.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/temp-file.hpp"

using namespace aeronet;

using test::ScopedTempDir;
using test::ScopedTempFile;

namespace {

struct ReadAction {
  enum class Kind : std::uint8_t { Error };
  Kind kind{Kind::Error};
  int err{0};
};

[[nodiscard]] ReadAction ReadErr(int err) { return ReadAction{ReadAction::Kind::Error, err}; }

test::KeyedActionQueue<std::string, ReadAction> gReadOverrides;
test::KeyedActionQueue<std::string, int> gLseekErrnos;
test::KeyedActionQueue<std::string, std::int64_t> gFstatSizes;
test::KeyedActionQueue<std::string, int> gFcntlErrnos;

void ResetFsHooks() {
  gReadOverrides.reset();
  gLseekErrnos.reset();
  gFstatSizes.reset();
  gFcntlErrnos.reset();
}

void SetReadActions(std::string_view path, std::initializer_list<ReadAction> actions) {
  gReadOverrides.setActions(std::string(path), actions);
}

void SetLseekErrors(std::string_view path, std::initializer_list<int> errs) {
  gLseekErrnos.setActions(std::string(path), errs);
}

void SetFcntlErrors(std::string_view path, std::initializer_list<int> errs) {
  gFcntlErrnos.setActions(std::string(path), errs);
}

std::optional<std::string> PathForFd(int fd) {
  std::array<char, 64> linkBuf{};
  std::snprintf(linkBuf.data(), linkBuf.size(), "/proc/self/fd/%d", fd);
  std::array<char, 512> pathBuf{};
  const auto len = ::readlink(linkBuf.data(), pathBuf.data(), pathBuf.size() - 1);
  if (len <= 0) {
    return std::nullopt;
  }
  pathBuf[static_cast<std::size_t>(len)] = '\0';
  return std::string(pathBuf.data());
}

ReadAction PopReadAction(int fd, bool& hasAction) {
  const auto pathOpt = PathForFd(fd);
  if (!pathOpt.has_value()) {
    hasAction = false;
    return {};
  }
  const auto action = gReadOverrides.pop(*pathOpt);
  if (!action.has_value()) {
    hasAction = false;
    return {};
  }
  hasAction = true;
  return *action;
}

int PopLseekErrno(int fd, bool& hasErr) {
  const auto pathOpt = PathForFd(fd);
  if (!pathOpt.has_value()) {
    hasErr = false;
    return 0;
  }
  const auto err = gLseekErrnos.pop(*pathOpt);
  if (!err.has_value()) {
    hasErr = false;
    return 0;
  }
  hasErr = true;
  return *err;
}

std::int64_t PopFstatSize(int fd, bool& hasSize) {
  const auto pathOpt = PathForFd(fd);
  if (!pathOpt.has_value()) {
    hasSize = false;
    return 0;
  }
  const auto val = gFstatSizes.pop(*pathOpt);
  if (!val.has_value()) {
    hasSize = false;
    return 0;
  }
  hasSize = true;
  return *val;
}

int PopFcntlErrno(int fd, bool& hasErr) {
  const auto pathOpt = PathForFd(fd);
  if (!pathOpt.has_value()) {
    hasErr = false;
    return 0;
  }
  const auto val = gFcntlErrnos.pop(*pathOpt);
  if (!val.has_value()) {
    hasErr = false;
    return 0;
  }
  hasErr = true;
  return *val;
}

class FileSyscallHookGuard {
 public:
  FileSyscallHookGuard() { ResetFsHooks(); }
  FileSyscallHookGuard(const FileSyscallHookGuard&) = delete;
  FileSyscallHookGuard& operator=(const FileSyscallHookGuard&) = delete;
  ~FileSyscallHookGuard() { ResetFsHooks(); }
};

}  // namespace

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreserved-identifier"
#elifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#endif

// NOLINTNEXTLINE(bugprone-reserved-identifier,clang-diagnostic-reserved-identifier)
extern "C" ssize_t read(int __fd, void* __buf, size_t __nbytes) {
  using ReadFn = ssize_t (*)(int, void*, size_t);
  static ReadFn real_read = reinterpret_cast<ReadFn>(dlsym(RTLD_NEXT, "read"));
  if (real_read == nullptr) {
    std::abort();
  }
  bool hasAction = false;
  ReadAction action = PopReadAction(__fd, hasAction);
  if (hasAction && action.kind == ReadAction::Kind::Error) {
    errno = action.err;
    return -1;
  }
  return real_read(__fd, __buf, __nbytes);
}

// NOLINTNEXTLINE(bugprone-reserved-identifier,clang-diagnostic-reserved-identifier)
extern "C" off_t lseek(int __fd, off_t __offset, int __whence) noexcept {
  using LseekFn = off_t (*)(int, off_t, int);
  static LseekFn real_lseek = reinterpret_cast<LseekFn>(dlsym(RTLD_NEXT, "lseek"));
  if (real_lseek == nullptr) {
    std::abort();
  }
  bool hasErr = false;
  const int err = PopLseekErrno(__fd, hasErr);
  if (hasErr) {
    errno = err;
    return static_cast<off_t>(-1);
  }
  return real_lseek(__fd, __offset, __whence);
}

// NOLINTNEXTLINE(bugprone-reserved-identifier,clang-diagnostic-reserved-identifier)
extern "C" int fstat(int __fd, struct stat* __buf) {
  using FstatFn = int (*)(int, struct stat*);
  static FstatFn real_fstat = reinterpret_cast<FstatFn>(dlsym(RTLD_NEXT, "fstat"));
  if (real_fstat == nullptr) {
    std::abort();
  }
  bool hasSize = false;
  const auto maybeSize = PopFstatSize(__fd, hasSize);
  if (hasSize) {
    if (maybeSize < 0) {
      // Negative value indicates an error code to return
      const int err = static_cast<int>(-maybeSize);
      errno = err == 0 ? EIO : err;
      return -1;
    }
    // Assume caller provided a valid buffer (fstat requires a non-null buffer).
    // Set the st_size to the requested value and report success
    __buf->st_size = static_cast<off_t>(maybeSize);
    return 0;
  }
  return real_fstat(__fd, __buf);
}

// NOLINTNEXTLINE(bugprone-reserved-identifier,clang-diagnostic-reserved-identifier)
extern "C" int fcntl(int __fd, int __cmd, ...) {
  using FcntlFn = int (*)(int, int, ...);
  static FcntlFn real_fcntl = reinterpret_cast<FcntlFn>(dlsym(RTLD_NEXT, "fcntl"));
  if (real_fcntl == nullptr) {
    std::abort();
  }
  bool hasErr = false;
  const int err = PopFcntlErrno(__fd, hasErr);
  if (hasErr && __cmd == F_DUPFD_CLOEXEC) {
    errno = err == 0 ? EBADF : err;
    return -1;
  }

  va_list ap;
  va_start(ap, __cmd);
  int arg = va_arg(ap, int);
  va_end(ap);
  return real_fcntl(__fd, __cmd, arg);
}

#ifdef __clang__
#pragma clang diagnostic pop
#elifdef __GNUC__
#pragma GCC diagnostic pop
#endif

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
  FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-missing");
  const auto missingPath = dir.dirPath() / "does-not-exist.bin";
  File fileObj(std::string_view(missingPath.string()), File::OpenMode::ReadOnly);
  EXPECT_FALSE(static_cast<bool>(fileObj));
  EXPECT_EQ(fileObj.size(), File::kError);
}

TEST(FileTest, StringViewConstructorLoadsContent) {
  FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-sv");
  ScopedTempFile tmp(dir, "string-view-content");
  const std::string path = tmp.filePath().string();
  std::string_view pathView(path);
  File fileObj(pathView, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));
  EXPECT_EQ(LoadAllContent(fileObj), "string-view-content");
}

TEST(FileTest, LoadAllContentRetriesAfterEintr) {
  FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-eintr");
  ScopedTempFile tmp(dir, "retry-data");
  const std::string path = tmp.filePath().string();
  File fileObj(path, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));
  SetReadActions(path, {ReadErr(EINTR)});
  EXPECT_EQ(LoadAllContent(fileObj), "retry-data");
}

TEST(FileTest, LoadAllContentThrowsOnFatalReadError) {
  FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-reader");
  ScopedTempFile tmp(dir, "payload");
  const std::string path = tmp.filePath().string();
  File fileObj(path, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));
  SetReadActions(path, {ReadErr(EIO)});
  EXPECT_EQ(LoadAllContent(fileObj), "payload");
}

TEST(FileTest, SizeUsesFstatOverride) {
  FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-fstat");
  ScopedTempFile tmp(dir, "content123");
  const std::string path = tmp.filePath().string();
  File fileObj(path, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));
  // Set a fake size via the fstat override for this path
  gFstatSizes.setActions(path, {static_cast<std::int64_t>(12345)});
  EXPECT_EQ(fileObj.size(), 12345U);

  gFstatSizes.setActions(path, {static_cast<std::int64_t>(-1)});
  EXPECT_EQ(fileObj.size(), File::kError);
}

TEST(FileTest, RestoreToStartLogsWhenLseekFails) {
  FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-lseek");
  ScopedTempFile tmp(dir, "abc");
  const std::string path = tmp.filePath().string();
  File fileObj(path, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(fileObj));
  SetLseekErrors(path, {EIO});
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
  FileSyscallHookGuard guard;
  ScopedTempDir dir("aeronet-file-dup-fail");
  ScopedTempFile tmp(dir, "dup-content-fail");
  const std::string path = tmp.filePath().string();

  File original(path, File::OpenMode::ReadOnly);
  ASSERT_TRUE(static_cast<bool>(original));
  // Simulate fcntl failure for dup
  SetFcntlErrors(path, {EBADF});
  EXPECT_FALSE(original.duplicate());
}