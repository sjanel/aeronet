
#include "aeronet/connection.hpp"

#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "aeronet/base-fd.hpp"
#include "aeronet/log.hpp"
#include "aeronet/socket.hpp"

namespace aeronet {

namespace {
int ComputeConnectionFd(int socketFd) {
  sockaddr_in in_addr{};
  socklen_t in_len = sizeof(in_addr);
  int fd = ::accept4(socketFd, reinterpret_cast<sockaddr*>(&in_addr), &in_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (fd < 0) {
    const auto savedErr = errno;  // capture errno before any other call
    if (savedErr == EAGAIN || savedErr == EWOULDBLOCK) {
      log::trace("Connection accept would block: {} - this is expected if no pending connections",
                 std::strerror(savedErr));
    } else {
      log::error("Connection accept failed for socket fd # {}: {}", socketFd, std::strerror(savedErr));
    }
    fd = -1;
  } else {
    log::debug("Connection fd # {} opened", fd);
  }
  return fd;
}

}  // namespace

Connection::Connection(const Socket& socket) : _baseFd(ComputeConnectionFd(socket.fd())) {}

Connection::Connection(BaseFd&& bd) noexcept : _baseFd(std::move(bd)) {}

}  // namespace aeronet
