#include "aeronet/file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/log.hpp"
#include "aeronet/mime-mappings.hpp"

namespace aeronet {

namespace {

inline int Flags(File::OpenMode mode) {
  switch (mode) {
    case File::OpenMode::ReadOnly:
      return O_RDONLY | O_CLOEXEC;
    default:
      throw std::invalid_argument("Unsupported File::OpenMode");
  }
}

inline void CheckFd(std::string_view path, int fd) {
  if (fd == -1) [[unlikely]] {
    log::error("Unable to open file '{}' (errno {}: {})", path, errno, std::strerror(errno));
  }
}

inline int CreateFileBaseFd(std::string_view path, File::OpenMode mode) {
  const int fd = ::open(std::string(path).c_str(), Flags(mode));
  CheckFd(path, fd);
  return fd;
}

inline int CreateFileBaseFd(const char* path, File::OpenMode mode) {
  const int fd = ::open(path, Flags(mode));
  CheckFd(path, fd);
  return fd;
}
}  // namespace

File::File(std::string_view path, OpenMode mode)
    : _fd(CreateFileBaseFd(path, mode)), _mimeMappingIdx(DetermineMIMETypeIdx(path)) {}

File::File(const char* path, OpenMode mode)
    : _fd(CreateFileBaseFd(path, mode)), _mimeMappingIdx(DetermineMIMETypeIdx(path)) {}

// Private helper: construct from existing fd (takes ownership)
File::File(BaseFd&& baseFd, MIMETypeIdx idx) noexcept : _fd(std::move(baseFd)), _mimeMappingIdx(idx) {}

std::size_t File::size() const {
  struct stat st{};
  if (_fd && ::fstat(_fd.fd(), &st) == 0) {
    return static_cast<std::size_t>(st.st_size);
  }
  return kError;
}

std::size_t File::readAt(std::span<std::byte> dst, std::size_t offset) const {
  for (;;) {
    const auto readResult = ::pread(_fd.fd(), dst.data(), dst.size(), static_cast<off_t>(offset));
    if (readResult >= 0) {
      return static_cast<std::size_t>(readResult);
    }
    if (errno == EINTR) {
      continue;
    }
    log::error("Unable to pread file (fd {}, offset {}, len {}): errno {}: {}", _fd.fd(), offset, dst.size(), errno,
               std::strerror(errno));
    return kError;
  }
}

File File::duplicate() const {
  // Duplicate file descriptor with CLOEXEC to avoid leaking across exec
  BaseFd newFd(::fcntl(_fd.fd(), F_DUPFD_CLOEXEC, 0));
  if (!newFd) {
    log::error("File::dup failed to dup fd {}: errno={} msg={}", _fd.fd(), errno, std::strerror(errno));
  }
  return File(std::move(newFd), _mimeMappingIdx);
}

std::string_view File::detectedContentType() const {
  if (_mimeMappingIdx == kUnknownMIMEMappingIdx) {
    return http::ContentTypeApplicationOctetStream;
  }
  return kMIMEMappings[_mimeMappingIdx].mimeType;
}

}  // namespace aeronet
