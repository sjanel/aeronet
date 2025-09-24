#include "base-fd.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "log.hpp"

namespace aeronet {

BaseFd::BaseFd(BaseFd&& other) noexcept : _fd(std::exchange(other._fd, -1)) {}

BaseFd& BaseFd::operator=(BaseFd&& other) noexcept {
  if (this != &other) {
    close();
    _fd = std::exchange(other._fd, -1);
  }
  return *this;
}

BaseFd::~BaseFd() { close(); }

int BaseFd::close() noexcept {
  if (_fd == -1) {
    // already closed
    return _fd;
  }
  int rc;
  while (true) {
    rc = ::close(_fd);
    if (rc == 0) {
      // success
      return _fd = -1;
    }
    if (errno == EINTR) {
      // Retry close if interrupted; POSIX allows either retry or treat as closed.
      continue;
    }
    // Other errors: EBADF (logic bug), ENOSPC (should not happen here), etc.
    log::error("Socket close(fd={}) failed: {}", _fd, std::strerror(errno));
    break;
  }
  _fd = -1;
  return _fd;
}

}  // namespace aeronet