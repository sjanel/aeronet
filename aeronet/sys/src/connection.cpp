
#include "connection.hpp"

#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>

#include "base-fd.hpp"
#include "log.hpp"
#include "socket.hpp"

namespace aeronet {

namespace {
int ComputeConnectionFd(int socketFd) {
  sockaddr_in in_addr{};
  socklen_t in_len = sizeof(in_addr);
  int fd = ::accept(socketFd, reinterpret_cast<sockaddr*>(&in_addr), &in_len);
  if (fd < 0) {
    int savedErr = errno;  // capture errno before any other call
    if (savedErr == EAGAIN || savedErr == EWOULDBLOCK) {
      log::debug("Connection accept would block: {}", std::strerror(savedErr));
    } else {
      log::error("Connection accept failed: {}", std::strerror(savedErr));
    }
    fd = -1;
  }
  return fd;
}

}  // namespace

Connection::Connection(const Socket& socket) : BaseFd(ComputeConnectionFd(socket.fd())) {}

}  // namespace aeronet
