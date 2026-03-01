#include "aeronet/socket.hpp"

#include "aeronet/platform.hpp"

#ifdef AERONET_WINDOWS
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include <cerrno>
#include <cstdint>
#include <stdexcept>

#include "aeronet/base-fd.hpp"
#include "aeronet/errno-throw.hpp"
#include "aeronet/log.hpp"

#ifndef AERONET_LINUX
#include "aeronet/socket-ops.hpp"  // SetNonBlocking, SetCloseOnExec
#endif

namespace aeronet {

namespace {

NativeHandle CreateSocket(Socket::Type type, int protocol) {
  switch (type) {
    case Socket::Type::Stream:
    case Socket::Type::StreamNonBlock:
      break;
    default:
      throw std::invalid_argument("Invalid Socket::Type");
  }

#ifdef AERONET_LINUX
  int sockType = SOCK_STREAM;
  if (type == Socket::Type::StreamNonBlock) {
    sockType |= SOCK_NONBLOCK | SOCK_CLOEXEC;
  }
  return ::socket(AF_INET, sockType, protocol);
#elifdef AERONET_WINDOWS
  SOCKET sock = ::WSASocketW(AF_INET, SOCK_STREAM, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);
  if (sock != INVALID_SOCKET && type == Socket::Type::StreamNonBlock) {
    u_long mode = 1;
    ::ioctlsocket(sock, FIONBIO, &mode);
  }
  return sock;
#else
  NativeHandle sock = ::socket(AF_INET, SOCK_STREAM, protocol);
  if (sock != kInvalidHandle && type == Socket::Type::StreamNonBlock) {
    SetNonBlocking(sock);
    SetCloseOnExec(sock);
    SetNoSigPipe(sock);
  }
  return sock;
#endif
}

}  // namespace

Socket::Socket(Type type, int protocol) : _baseFd(CreateSocket(type, protocol)) {
  if (!_baseFd) {
    ThrowSystemError("Unable to create a new socket");
  }
  log::debug("Socket fd # {} opened", static_cast<intptr_t>(_baseFd.fd()));
}

[[nodiscard]] bool Socket::tryBind(bool reusePort, bool tcpNoDelay, uint16_t port) const {
  const auto fd = _baseFd.fd();

  static constexpr int enable = 1;
#ifdef AERONET_WINDOWS
  const char* optPtr = reinterpret_cast<const char*>(&enable);
#else
  const int* optPtr = &enable;
#endif

  // NOLINTNEXTLINE(misc-include-cleaner) sys/socket.h is the correct header for SOL_SOCKET and SO_REUSEADDR
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, optPtr, sizeof(enable)) == -1) {
    ThrowSystemError("setsockopt(SO_REUSEADDR) failed");
  }
#ifdef AERONET_POSIX
  // SO_REUSEPORT: kernel load-balancing across listeners (Linux 3.9+, macOS 12+).
  // NOLINTNEXTLINE(misc-include-cleaner) sys/socket.h is the correct header for SOL_SOCKET and SO_REUSEPORT
  if (reusePort && ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, optPtr, sizeof(enable)) == -1) {
    ThrowSystemError("setsockopt(SO_REUSEPORT) failed");
  }
#else
  (void)reusePort;  // SO_REUSEPORT not available on Windows
#endif
  if (tcpNoDelay && ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, optPtr, sizeof(enable)) == -1) {
    ThrowSystemError("setsockopt(TCP_NODELAY) failed");
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
    log::warn("Socket fd # {} bind to port {} failed: {}", fd, port, SystemErrorMessage(LastSystemError()));
    return false;
  }
  return true;
}

void Socket::bindAndListen(bool reusePort, bool tcpNoDelay, uint16_t& port) {
  const int fd = _baseFd.fd();

  if (!tryBind(reusePort, tcpNoDelay, port)) {
    ThrowSystemError("bind failed");
  }
  if (::listen(fd, SOMAXCONN) == -1) {
    ThrowSystemError("listen failed");
  }
  if (port == 0) {
    sockaddr_in actual{};
    socklen_t alen = sizeof(actual);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &alen) == -1) {
      ThrowSystemError("getsockname failed");
    }
    port = ntohs(actual.sin_port);
  }
}

}  // namespace aeronet