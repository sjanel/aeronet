#include "aeronet/socket-ops.hpp"

#include "aeronet/platform.hpp"

#ifdef AERONET_WINDOWS
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include <cstddef>

namespace aeronet {

bool SetNonBlocking(NativeHandle fd) noexcept {
#ifdef AERONET_WINDOWS
  u_long mode = 1;
  return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return false;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

bool SetCloseOnExec(NativeHandle fd) noexcept {
#ifdef AERONET_WINDOWS
  // Windows does not have a close-on-exec concept for sockets.
  (void)fd;
  return true;
#else
  const int flags = ::fcntl(fd, F_GETFD, 0);
  if (flags == -1) {
    return false;
  }
  return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != -1;
#endif
}

bool SetNoSigPipe(NativeHandle fd) noexcept {
#ifdef AERONET_MACOS
  static constexpr int kEnable = 1;
  return ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &kEnable, sizeof(kEnable)) == 0;
#else
  // Linux uses MSG_NOSIGNAL per-send; Windows has no SIGPIPE concept.
  (void)fd;
  return true;
#endif
}

#ifdef AERONET_POSIX
void SetPipeNonBlockingCloExec(int pipeRd, int pipeWr) noexcept {
  for (int pfd : {pipeRd, pipeWr}) {
    int flags = ::fcntl(pfd, F_GETFL, 0);
    if (flags != -1) {
      ::fcntl(pfd, F_SETFL, flags | O_NONBLOCK);
    }
    int fdFlags = ::fcntl(pfd, F_GETFD, 0);
    if (fdFlags != -1) {
      ::fcntl(pfd, F_SETFD, fdFlags | FD_CLOEXEC);
    }
  }
}
#endif

bool SetTcpNoDelay(NativeHandle fd) noexcept {
  static constexpr int kEnable = 1;
#ifdef AERONET_WINDOWS
  return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&kEnable), sizeof(kEnable)) == 0;
#else
  return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &kEnable, sizeof(kEnable)) == 0;
#endif
}

int GetSocketError(NativeHandle fd) noexcept {
  int err = 0;
  socklen_t len = sizeof(err);
#ifdef AERONET_WINDOWS
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) == -1) {
    return LastSystemError();
  }
#else
  // NOLINTNEXTLINE(misc-include-cleaner) sys/socket.h is the correct header for SOL_SOCKET and SO_ERROR
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == -1) {
    return LastSystemError();
  }
#endif
  return err;
}

bool GetLocalAddress(NativeHandle fd, sockaddr_storage& addr) noexcept {
  socklen_t len = sizeof(addr);
  return ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0;
}

bool GetPeerAddress(NativeHandle fd, sockaddr_storage& addr) noexcept {
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

int64_t SafeSend(NativeHandle fd, const void* data, std::size_t len) noexcept {
#ifdef AERONET_LINUX
  return static_cast<int64_t>(::send(fd, data, len, MSG_NOSIGNAL));
#elifdef AERONET_MACOS
  // macOS uses SO_NOSIGPIPE socket option (set at socket creation) instead of MSG_NOSIGNAL.
  return static_cast<int64_t>(::send(fd, data, len, 0));
#elifdef AERONET_WINDOWS
  return static_cast<int64_t>(::send(fd, static_cast<const char*>(data), static_cast<int>(len), 0));
#else
  return static_cast<int64_t>(::send(fd, data, len, 0));
#endif
}

bool ShutdownWrite(NativeHandle fd) noexcept {
#ifdef AERONET_WINDOWS
  return ::shutdown(fd, SD_SEND) == 0;
#else
  return ::shutdown(fd, SHUT_WR) == 0;
#endif
}

bool ShutdownReadWrite(NativeHandle fd) noexcept {
#ifdef AERONET_WINDOWS
  return ::shutdown(fd, SD_BOTH) == 0;
#else
  return ::shutdown(fd, SHUT_RDWR) == 0;
#endif
}

}  // namespace aeronet
