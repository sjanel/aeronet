#include "aeronet/socket.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "aeronet/base-fd.hpp"
#include "aeronet/errno-throw.hpp"
#include "aeronet/log.hpp"

namespace aeronet {

namespace {

int ComputeSocketType(Socket::Type type) {
  switch (type) {
    case Socket::Type::Stream:
      return SOCK_STREAM;
    case Socket::Type::StreamNonBlock:
      return SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
    default:
      throw std::invalid_argument("Invalid socket type");
  }
}

}  // namespace

Socket::Socket(Type type, int protocol) : _baseFd(::socket(AF_INET, ComputeSocketType(type), protocol)) {
  if (_baseFd.fd() == -1) {
    throw_errno("Unable to create a new socket");
  }
  log::debug("Socket fd # {} opened", _baseFd.fd());
}

[[nodiscard]] bool Socket::tryBind(bool reusePort, bool tcpNoDelay, uint16_t port) const {
  const int fd = _baseFd.fd();

  static constexpr int enable = 1;
  // NOLINTNEXTLINE(misc-include-cleaner) sys/socket.h is the correct header for SOL_SOCKET and SO_REUSEADDR
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) == -1) {
    throw_errno("setsockopt(SO_REUSEADDR) failed");
  }
  // NOLINTNEXTLINE(misc-include-cleaner) sys/socket.h is the correct header for SOL_SOCKET and SO_REUSEPORT
  if (reusePort && ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable)) == -1) {
    throw_errno("setsockopt(SO_REUSEPORT) failed");
  }
  if (tcpNoDelay && ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)) == -1) {
    throw_errno("setsockopt(TCP_NODELAY) failed");
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == -1) {
    log::warn("Socket fd # {} bind to port {} failed: {}", fd, port, std::strerror(errno));
    return false;
  }
  return true;
}

void Socket::bindAndListen(bool reusePort, bool tcpNoDelay, uint16_t &port) {
  const int fd = _baseFd.fd();

  if (!tryBind(reusePort, tcpNoDelay, port)) {
    throw_errno("bind failed");
  }
  if (::listen(fd, SOMAXCONN) == -1) {
    throw_errno("listen failed");
  }
  if (port == 0) {
    sockaddr_in actual{};
    socklen_t alen = sizeof(actual);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&actual), &alen) == -1) {
      throw_errno("getsockname failed");
    }
    port = ntohs(actual.sin_port);
  }
}

}  // namespace aeronet