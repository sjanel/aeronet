#include "socket.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "base-fd.hpp"
#include "exception.hpp"

namespace aeronet {

namespace {

constexpr int ComputeSocketType(Socket::Type socketType) {
  switch (socketType) {
    case Socket::Type::STREAM:
      return SOCK_STREAM;
    case Socket::Type::DATAGRAM:
      return SOCK_DGRAM;
    default:
      std::unreachable();
  }
}

}  // namespace

Socket::Socket(Type type, int protocol) : BaseFd(::socket(AF_INET, ComputeSocketType(type), protocol)) {
  if (this->fd() < 0) {
    throw exception("Unable to create a new socket, with error {}", std::strerror(errno));
  }
}

}  // namespace aeronet