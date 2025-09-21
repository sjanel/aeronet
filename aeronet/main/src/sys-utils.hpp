#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string_view>

#include "log.hpp"

namespace aeronet {

// setNonBlocking
//  Sets O_NONBLOCK on a file descriptor.
//  Returns 0 on success, -1 on failure (errno preserved).
inline int setNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return -1;
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    return -1;
  }
  return 0;
}

// safeClose
//  Defensive close wrapper.
//  Rationale:
//    close() may report EINTR if interrupted by a signal before completing. Retrying avoids the (rare) situation
//    where callers assume a descriptor was released when it was not. Most modern Linux kernels seldom produce EINTR
//    for close(), but POSIX permits it and the retry loop is inexpensive.
//  Behavior:
//    - Retries automatically on EINTR.
//    - Logs any non-EINTR failure (e.g., EBADF) with context for diagnostics.
//  Returns 0 on success, -1 on persistent failure.
inline int safeClose(int fd, std::string_view context) {
  if (fd < 0) {
    return 0;
  }
  while (true) {
    if (::close(fd) == 0) {
      return 0;
    }
    if (errno == EINTR) {
      continue;  // retry close
    }
    log::error("close({} fd={}) failed: {}", context, fd, std::strerror(errno));
    return -1;
  }
}

}  // namespace aeronet
