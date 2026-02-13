#include "aeronet/socket-ops.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>

// POSIX headers needed on Linux and macOS.
// Windows will need a separate implementation block later.
#include <fcntl.h>

namespace aeronet {

bool SetNonBlocking(int fd) noexcept {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return false;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

bool SetCloseOnExec(int fd) noexcept {
  const int flags = ::fcntl(fd, F_GETFD, 0);
  if (flags == -1) {
    return false;
  }
  return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != -1;
}

bool SetTcpNoDelay(int fd) noexcept {
  static constexpr int kEnable = 1;
  return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &kEnable, sizeof(kEnable)) == 0;
}

int GetSocketError(int fd) noexcept {
  int err = 0;
  socklen_t len = sizeof(err);
  // NOLINTNEXTLINE(misc-include-cleaner) sys/socket.h is the correct header for SOL_SOCKET and SO_ERROR
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == -1) {
    return errno;
  }
  return err;
}

bool GetLocalAddress(int fd, sockaddr_storage& addr) noexcept {
  socklen_t len = sizeof(addr);
  return ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0;
}

bool GetPeerAddress(int fd, sockaddr_storage& addr) noexcept {
  socklen_t len = sizeof(addr);
  return ::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0;
}

bool IsLoopback(const sockaddr_storage& addr) noexcept {
  if (addr.ss_family == AF_INET) {
    const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
    // 127.0.0.0/8
    return (ntohl(in->sin_addr.s_addr) & 0xFF000000U) == 0x7F000000U;
  }
  if (addr.ss_family == AF_INET6) {
    const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
    return IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
  }
  return false;
}

int64_t SafeSend(int fd, const void* data, std::size_t len) noexcept {
#ifdef __linux__
  return static_cast<int64_t>(::send(fd, data, len, MSG_NOSIGNAL));
#elifdef __APPLE__
  // macOS uses SO_NOSIGPIPE socket option (set at socket creation) instead of MSG_NOSIGNAL.
  return static_cast<int64_t>(::send(fd, data, len, 0));
#else
  return static_cast<int64_t>(::send(fd, data, len, 0));
#endif
}

}  // namespace aeronet
