#include "aeronet/file.hpp"

#include "aeronet/platform.hpp"

#ifdef AERONET_POSIX
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#elifdef AERONET_WINDOWS
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "aeronet/base-fd.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/log.hpp"
#include "aeronet/mime-mappings.hpp"

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

inline void CheckFd(std::string_view path, int fd) {
  if (fd == -1) [[unlikely]] {
    log::error("Unable to open file '{}' (error {}: {})", path, errno, SystemErrorMessage(errno));
  }
}

inline int CreateFileBaseFd(std::string_view path, File::OpenMode mode) {
#ifdef AERONET_POSIX
  const int fd = ::open(std::string(path).c_str(), Flags(mode));
#elifdef AERONET_WINDOWS
  const int fd = ::_open(std::string(path).c_str(), Flags(mode));
#endif
  CheckFd(path, fd);
  return fd;
}

inline int CreateFileBaseFd(const char* path, File::OpenMode mode) {
#ifdef AERONET_POSIX
  const int fd = ::open(path, Flags(mode));
#elifdef AERONET_WINDOWS
  const int fd = ::_open(path, Flags(mode));
#endif
  CheckFd(path, fd);
  return fd;
}

inline std::size_t GetFileSize(BaseFd& fd) {
#ifdef AERONET_POSIX
  struct stat st{};
  if (fd && ::fstat(fd.fd(), &st) == 0) {
    return static_cast<std::size_t>(st.st_size);
  }
#elifdef AERONET_WINDOWS
  struct _stat64 st{};
  if (fd && ::_fstat64(fd.fd(), &st) == 0) {
    return static_cast<std::size_t>(st.st_size);
  }
#endif
  fd.close();
  return File::kError;
}

}  // namespace

File::File(std::string_view path, OpenMode mode)
    : _fd(CreateFileBaseFd(path, mode)), _mimeMappingIdx(DetermineMIMETypeIdx(path)), _fileSize(GetFileSize(_fd)) {}

File::File(const char* path, OpenMode mode)
    : _fd(CreateFileBaseFd(path, mode)), _mimeMappingIdx(DetermineMIMETypeIdx(path)), _fileSize(GetFileSize(_fd)) {}

std::size_t File::readAt(std::span<std::byte> dst, std::size_t offset) const {
  for (;;) {
#ifdef AERONET_POSIX
    const auto readResult = ::pread(_fd.fd(), dst.data(), dst.size(), static_cast<off_t>(offset));
#elifdef AERONET_WINDOWS
    // Windows has no pread(); emulate by seeking + reading (file descriptors are not shared across threads here).
    if (::_lseeki64(_fd.fd(), static_cast<__int64>(offset), SEEK_SET) == -1) {
      log::error("Unable to seek file (fd {}, offset {}): error {}: {}", _fd.fd(), offset, errno,
                 SystemErrorMessage(errno));
      return kError;
    }
    const auto readResult = ::_read(_fd.fd(), dst.data(), static_cast<unsigned int>(dst.size()));
#endif
    if (readResult >= 0) {
      return static_cast<std::size_t>(readResult);
    }
    if (errno == EINTR) {
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
