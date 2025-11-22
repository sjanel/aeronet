#include "aeronet/base-fd.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "aeronet/log.hpp"

namespace aeronet {

BaseFd& BaseFd::operator=(BaseFd&& other) noexcept {
  if (this != &other) {
    close();
    _fd = other.release();
  }
  return *this;
}

void BaseFd::close() noexcept {
  if (_fd != kClosedFd) {
    while (true) {
      if (::close(_fd) == 0) {
        // success
        break;
      }
      if (errno == EINTR) {
        // Retry close if interrupted; POSIX allows either retry or treat as closed.
        continue;
      }
      // Other errors: EBADF (benign if race closed elsewhere), ENOSPC (should not happen here), etc.
      log::error("close fd # {} failed: {}", _fd, std::strerror(errno));
      break;
    }
    log::debug("fd # {} closed", _fd);
    _fd = kClosedFd;
  }
}

int BaseFd::release() noexcept { return std::exchange(_fd, kClosedFd); }

}  // namespace aeronet