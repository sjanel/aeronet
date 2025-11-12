#include "aeronet/socket.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <cstring>

#include "aeronet/base-fd.hpp"
#include "aeronet/errno_throw.hpp"
#include "aeronet/log.hpp"

namespace aeronet {

Socket::Socket(int type, int protocol) : _baseFd(::socket(AF_INET, type, protocol)) {
  if (_baseFd.fd() == -1) {
    throw_errno("Unable to create a new socket");
  }
  log::debug("Socket fd # {} opened", _baseFd.fd());
}

}  // namespace aeronet