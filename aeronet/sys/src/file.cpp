#include "file.hpp"

#include <fcntl.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "exception.hpp"

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

}  // namespace aeronet
