#include "socket.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <cstring>

#include "base-fd.hpp"
#include "exception.hpp"
#include "log.hpp"

namespace aeronet {

Socket::Socket(int type, int protocol) : _baseFd(::socket(AF_INET, type, protocol)) {
  if (_baseFd.fd() < 0) {
    throw exception("Unable to create a new socket, with error {}", std::strerror(errno));
  }
  log::debug("Socket fd={} opened", _baseFd.fd());
}

}  // namespace aeronet