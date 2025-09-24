#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

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

}  // namespace aeronet
