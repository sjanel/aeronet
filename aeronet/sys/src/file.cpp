#include "file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "exception.hpp"
#include "log.hpp"

namespace aeronet {

namespace {

int Flags(File::OpenMode mode) {
  switch (mode) {
    case File::OpenMode::ReadOnly:
      return O_RDONLY | O_CLOEXEC;
    default:
      std::unreachable();
  }
}

void CheckFd(std::string_view path, int fd) {
  if (fd < 0) {
    throw exception("Unable to open file '{}' (errno {}: {})", path, errno, std::strerror(errno));
  }
}

int CreateFileBaseFd(std::string_view path, File::OpenMode mode) {
  int fd = ::open(std::string(path).c_str(), Flags(mode));
  CheckFd(path, fd);
  return fd;
}

int CreateFileBaseFd(const char* path, File::OpenMode mode) {
  int fd = ::open(path, Flags(mode));
  CheckFd(path, fd);
  return fd;
}

}  // namespace

File::File(std::string_view path, OpenMode mode) : _fd(CreateFileBaseFd(path, mode)) {}

File::File(const char* path, OpenMode mode) : _fd(CreateFileBaseFd(path, mode)) {}

std::size_t File::size() const {
  struct stat st{};
  if (_fd && ::fstat(_fd.fd(), &st) == 0) {
    return static_cast<std::uint64_t>(st.st_size);
  }
  throw std::runtime_error("File::size failed");
}

std::string File::loadAllContent() const {
  if (!_fd) {
    throw std::runtime_error("File is not opened");
  }

  std::string content;
  content.reserve(size());

  // Ensure we always leave the file offset at the start on return (normal or
  // exceptional). The user wants the file to be positioned at 0 when this
  // function exits, so use RAII to restore that state in the destructor.
  struct RestoreToStart {
    explicit RestoreToStart(int fd) noexcept : _fd(fd) {}

    RestoreToStart(const RestoreToStart&) = delete;
    RestoreToStart& operator=(const RestoreToStart&) = delete;
    RestoreToStart(RestoreToStart&&) = delete;
    RestoreToStart& operator=(RestoreToStart&&) = delete;

    ~RestoreToStart() noexcept {
      if (::lseek(_fd, 0, SEEK_SET) == -1) {
        // Log but do not throw from a destructor.
        log::error("File::loadAllContent: failed to restore offset for fd # {} errno={} msg={}", _fd, errno,
                   std::strerror(errno));
      }
    }

    int _fd;
  };

  RestoreToStart restore(_fd.fd());

  constexpr std::size_t kBufSize = 8192;
  for (;;) {
    const std::size_t oldSize = content.size();

    // We capture lastRead to inspect the result after the non-throwing lambda.
    ssize_t lastRead = 0;
    content.resize_and_overwrite(oldSize + kBufSize,
                                 [this, oldSize, &lastRead](char* data, [[maybe_unused]] std::size_t newCap) {
                                   lastRead = ::read(_fd.fd(), data + oldSize, kBufSize);
                                   if (lastRead > 0) {
                                     return oldSize + static_cast<std::size_t>(lastRead);
                                   }
                                   // On EOF or error, return oldSize to indicate no progress.
                                   return oldSize;
                                 });

    if (lastRead > 0) {
      continue;  // read more
    }
    if (lastRead == 0) {
      break;  // EOF
    }
    // lastRead < 0 -> error. EINTR should be retried.
    if (errno == EINTR) {
      continue;
    }
    throw exception("Unable to read file (fd {}): errno {}: {}", _fd.fd(), errno, std::strerror(errno));
  }

  return content;
}

}  // namespace aeronet
