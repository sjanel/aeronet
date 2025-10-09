#include "base-fd.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "log.hpp"

namespace aeronet {

BaseFd::BaseFd(BaseFd&& other) noexcept : _fd(std::exchange(other._fd, kClosedFd)) {}

BaseFd& BaseFd::operator=(BaseFd&& other) noexcept {
  if (this != &other) {
    close();
    _fd = std::exchange(other._fd, kClosedFd);
  }
  return *this;
}

BaseFd::~BaseFd() { close(); }

void BaseFd::close() noexcept {
  if (_fd == kClosedFd) {
    // already closed
    return;
  }
  while (true) {
    const auto errc = ::close(_fd);
    if (errc == 0) {
      // success
      break;
    }
    if (errno == EINTR) {
      // Retry close if interrupted; POSIX allows either retry or treat as closed.
      continue;
    }
    // Other errors: EBADF (benign if race closed elsewhere), ENOSPC (should not happen here), etc.
    log::error("close fd={} failed: {}", _fd, std::strerror(errno));
    break;
  }
  log::debug("fd={} closed", _fd);
  _fd = kClosedFd;
}

}  // namespace aeronet