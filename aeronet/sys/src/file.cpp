#include "aeronet/file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef AERONET_POSIX
#include <unistd.h>
#elifdef AERONET_WINDOWS
#include <io.h>
#include <windows.h>
#endif

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/base-fd.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/log.hpp"
#include "aeronet/mime-mappings.hpp"
#include "aeronet/system-error-message.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

namespace {

inline int Flags(File::OpenMode mode) {
  switch (mode) {
    case File::OpenMode::ReadOnly:
#ifdef AERONET_POSIX
      return O_RDONLY | O_CLOEXEC;
#elifdef AERONET_WINDOWS
      return _O_RDONLY | _O_BINARY;
#endif
    default:
      throw std::invalid_argument("Unsupported File::OpenMode");
  }
}

inline void CheckFileFd(std::string_view path, int fd) {
  if (fd == -1) [[unlikely]] {
    log::error("Unable to open file '{}' (error {}: {})", path, errno, SystemErrorMessage(errno));
  }
}

inline BaseFd CreateFileBaseFd(std::string_view path, File::OpenMode mode) {
#ifdef AERONET_POSIX
  const int fd = ::open(std::string(path).c_str(), Flags(mode));
#elifdef AERONET_WINDOWS
  const int fd = _open(std::string(path).c_str(), Flags(mode));
#endif
  CheckFileFd(path, fd);
#ifdef AERONET_WINDOWS
  return BaseFd(static_cast<NativeHandle>(fd), BaseFd::HandleKind::CrtFd);
#else
  return BaseFd(fd);
#endif
}

inline BaseFd CreateFileBaseFd(const char* path, File::OpenMode mode) {
#ifdef AERONET_POSIX
  const int fd = ::open(path, Flags(mode));
#elifdef AERONET_WINDOWS
  const int fd = _open(path, Flags(mode));
#endif
  CheckFileFd(path, fd);
#ifdef AERONET_WINDOWS
  return BaseFd(static_cast<NativeHandle>(fd), BaseFd::HandleKind::CrtFd);
#else
  return BaseFd(fd);
#endif
}

// Stat the descriptor once, filling both size and last-modification time. On failure the descriptor is
// closed, the size is set to File::kError and the mtime is left at the kInvalidTimePoint sentinel.
inline void StatFile(BaseFd& fd, std::size_t& sizeOut, SysTimePoint& mtimeOut, File::Identity& identity) {
#ifdef AERONET_POSIX
  struct stat st{};
  if (fd && ::fstat(fd.fd(), &st) == 0) {
    identity.device = static_cast<uint64_t>(st.st_dev);
    identity.inode = static_cast<uint64_t>(st.st_ino);
    sizeOut = static_cast<std::size_t>(st.st_size);
    // POSIX names the modification-time timespec `st_mtim`; Apple/BSD name it `st_mtimespec`.
#ifdef AERONET_MACOS
    const auto& mtimespec = st.st_mtimespec;
#else
    const auto& mtimespec = st.st_mtim;
#endif
    mtimeOut = SysTimePoint{std::chrono::duration_cast<SysDuration>(std::chrono::seconds{mtimespec.tv_sec} +
                                                                    std::chrono::nanoseconds{mtimespec.tv_nsec})};
    return;
  }
#elifdef AERONET_WINDOWS
  HANDLE handle = fd ? reinterpret_cast<HANDLE>(_get_osfhandle(static_cast<int>(fd.fd()))) : INVALID_HANDLE_VALUE;
  if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(handle, &info)) {
      identity.device = static_cast<uint64_t>(info.dwVolumeSerialNumber);
      identity.inode = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
      sizeOut = static_cast<std::size_t>((static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow);

      ULARGE_INTEGER uli{};
      uli.LowPart = info.ftLastWriteTime.dwLowDateTime;
      uli.HighPart = info.ftLastWriteTime.dwHighDateTime;
      constexpr uint64_t kFileTimeToUnixEpoch100ns = 116444736000000000ULL;
      using Ticks100ns = std::chrono::duration<int64_t, std::ratio<1, 10'000'000> >;
      mtimeOut = SysTimePoint{std::chrono::duration_cast<SysDuration>(
          Ticks100ns{static_cast<int64_t>(uli.QuadPart - kFileTimeToUnixEpoch100ns)})};
      return;
    }
  }
#endif
  fd.close();
  sizeOut = File::kError;
}

inline BaseFd DuplicateFileBaseFd(const BaseFd& src) {
  if (!src) {
    return BaseFd();
  }
#ifdef AERONET_POSIX
  const int fd = ::dup(src.fd());
  if (fd == -1) [[unlikely]] {
    log::error("Unable to duplicate file descriptor {} (error {}: {})", src.fd(), errno, SystemErrorMessage(errno));
  }
  return BaseFd(fd);
#elifdef AERONET_WINDOWS
  const int fd = _dup(static_cast<int>(src.fd()));
  if (fd == -1) [[unlikely]] {
    log::error("Unable to duplicate file descriptor {} (error {}: {})", static_cast<int>(src.fd()), errno,
               SystemErrorMessage(errno));
  }
  return BaseFd(static_cast<NativeHandle>(fd), BaseFd::HandleKind::CrtFd);
#endif
}

}  // namespace

File::File(std::string_view path, OpenMode mode)
    : _fd(CreateFileBaseFd(path, mode)), _mimeMappingIdx(DetermineMIMETypeIdx(path)) {
  StatFile(_fd, _fileSize, _mtime, _identity);
}

File::File(const char* path, OpenMode mode)
    : _fd(CreateFileBaseFd(path, mode)), _mimeMappingIdx(DetermineMIMETypeIdx(path)) {
  StatFile(_fd, _fileSize, _mtime, _identity);
}

File::File(const File& rhs)
    : _fd(DuplicateFileBaseFd(rhs._fd)),
      _mimeMappingIdx(rhs._mimeMappingIdx),
      _identity(rhs._identity),
      _fileSize(_fd ? rhs._fileSize : kError),
      _mtime(_fd ? rhs._mtime : SysTimePoint::max()) {}

File& File::operator=(const File& rhs) {
  if (this != &rhs) {
    BaseFd newFd = DuplicateFileBaseFd(rhs._fd);
    _mimeMappingIdx = rhs._mimeMappingIdx;
    _identity = rhs._identity;
    _fileSize = newFd ? rhs._fileSize : kError;
    _mtime = newFd ? rhs._mtime : SysTimePoint::max();
    _fd = std::move(newFd);  // old fd (if any) closed by BaseFd's move-assignment
  }
  return *this;
}

std::size_t File::readAt(std::span<std::byte> dst, std::size_t offset) const {
  for (;;) {
#ifdef AERONET_POSIX
    const auto readResult = ::pread(_fd.fd(), dst.data(), dst.size(), static_cast<off_t>(offset));
#elifdef AERONET_WINDOWS
    // Windows has no pread(); emulate by seeking + reading (file descriptors are not shared across threads here).
    if (_lseeki64(static_cast<int>(_fd.fd()), static_cast<__int64>(offset), SEEK_SET) == -1) {
      log::error("Unable to seek file (fd {}, offset {}): error {}: {}", static_cast<int>(_fd.fd()), offset, errno,
                 SystemErrorMessage(errno));
      return kError;
    }
    const auto readResult = _read(static_cast<int>(_fd.fd()), dst.data(), static_cast<unsigned int>(dst.size()));
#endif
    if (readResult >= 0) {
      return static_cast<std::size_t>(readResult);
    }
    if (errno == error::kInterrupted) {
      continue;
    }
    log::error("Unable to pread file (fd {}, offset {}, len {}): error {}: {}", _fd.fd(), offset, dst.size(), errno,
               SystemErrorMessage(errno));
    return kError;
  }
}

std::string_view File::detectedContentType() const {
  if (_mimeMappingIdx == kUnknownMIMEMappingIdx) {
    return http::ContentTypeApplicationOctetStream;
  }
  return kMIMEMappings[_mimeMappingIdx].mimeType;
}

}  // namespace aeronet
