#pragma once

#include "aeronet/platform.hpp"

#ifdef AERONET_POSIX
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

#include "aeronet/sys-test-support.hpp"

namespace aeronet::test {

struct ReadAction {
  enum class Kind : std::uint8_t { Error };
  Kind kind{Kind::Error};
  int err{0};
};

[[nodiscard]] inline ReadAction ReadErr(int err) { return ReadAction{ReadAction::Kind::Error, err}; }

inline test::KeyedActionQueue<std::string, ReadAction> gReadOverrides;
inline test::KeyedActionQueue<std::string, int> gLseekErrnos;
inline test::KeyedActionQueue<std::string, std::int64_t> gFstatSizes;
inline test::KeyedActionQueue<std::string, int> gFcntlErrnos;

inline void ResetFsHooks() {
  gReadOverrides.reset();
  gLseekErrnos.reset();
  gFstatSizes.reset();
  gFcntlErrnos.reset();
}

inline void SetReadActions(std::string_view path, std::initializer_list<ReadAction> actions) {
  gReadOverrides.setActions(std::string(path), actions);
}

inline void SetLseekErrors(std::string_view path, std::initializer_list<int> errs) {
  gLseekErrnos.setActions(std::string(path), errs);
}

inline void SetFcntlErrors(std::string_view path, std::initializer_list<int> errs) {
  gFcntlErrnos.setActions(std::string(path), errs);
}

#ifdef AERONET_FILE_SYS_TEST_SUPPORT_USE_EXISTING_PATHFORFD
using aeronet::test::PathForFd;
#else
inline std::optional<std::string> PathForFd(int fd) {
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
#endif

inline ReadAction PopReadAction(int fd, bool& hasAction) {
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

inline int PopLseekErrno(int fd, bool& hasErr) {
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

inline std::int64_t PopFstatSize(int fd, bool& hasSize) {
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

inline int PopFcntlErrno(int fd, bool& hasErr) {
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

}  // namespace aeronet::test

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
  aeronet::test::ReadAction action = aeronet::test::PopReadAction(__fd, hasAction);
  if (hasAction && action.kind == aeronet::test::ReadAction::Kind::Error) {
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
  const int err = aeronet::test::PopLseekErrno(__fd, hasErr);
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
  const auto maybeSize = aeronet::test::PopFstatSize(__fd, hasSize);
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
  const int err = aeronet::test::PopFcntlErrno(__fd, hasErr);
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