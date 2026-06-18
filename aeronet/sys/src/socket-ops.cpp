#include "aeronet/socket-ops.hpp"

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef AERONET_WINDOWS
#include <io.h>
#include <ws2tcpip.h>

#include <stdexcept>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#endif

#include "aeronet/native-handle.hpp"
#include "aeronet/system-error.hpp"

namespace aeronet {

#ifdef AERONET_WINDOWS
void EnsureWinsockInitialized() {
  struct WinsockInit {
    WinsockInit() {
      WSADATA wsaData;
      ::WSAStartup(MAKEWORD(2, 2), &wsaData);
    }
    ~WinsockInit() { ::WSACleanup(); }
  };
  static WinsockInit instance;
}
#endif

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
void SetPipeNonBlockingCloExec(NativeHandle pipeRd, NativeHandle pipeWr) noexcept {
  const NativeHandle sockets[]{pipeRd, pipeWr};
  for (NativeHandle pfd : sockets) {
    int flags = ::fcntl(pfd, F_GETFL, 0);
    assert(flags != -1);
    flags = ::fcntl(pfd, F_SETFL, flags | O_NONBLOCK);
    assert(flags != -1);

    flags = ::fcntl(pfd, F_GETFD, 0);
    assert(flags != -1);
    flags = ::fcntl(pfd, F_SETFD, flags | FD_CLOEXEC);
    assert(flags != -1);
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

bool SetTcpCork([[maybe_unused]] NativeHandle fd, [[maybe_unused]] bool enable) noexcept {
#ifdef AERONET_LINUX
  const int val = enable ? 1 : 0;
  return ::setsockopt(fd, IPPROTO_TCP, TCP_CORK, &val, sizeof(val)) == 0;
#else
  // macOS TCP_NOPUSH has different semantics: clearing the flag does NOT flush
  // accumulated data (unlike Linux TCP_CORK). Combined with TCP_NODELAY this
  // causes data to stall. writev already coalesces header+body on macOS.
  // Windows has no equivalent; coalescing relies on Nagle and WSASend scatter-gather.
  return true;
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

bool IsConnectionStale(NativeHandle fd) noexcept {
  // An idle, fully-consumed HTTP/1.1 keep-alive connection must be quiet: anything readable means do not
  // reuse it. EOF (peeked == 0) is an orderly close; a hard error is a reset; and any *pending bytes* are
  // unexpected on an idle connection — most importantly a TLS `close_notify`, which the peer sends (as an
  // encrypted record, i.e. readable data, not EOF) when it gracefully closes. Treating pending bytes as
  // stale therefore catches the TLS graceful-close case too; the only false positive is a TLS session
  // ticket that happened to arrive while idle, which merely costs one reconnect. Uses MSG_PEEK so nothing
  // is consumed.
  char probe = 0;
#ifdef AERONET_WINDOWS
  // Windows recv() has no MSG_DONTWAIT, so guarantee the peek below cannot block (regardless of the
  // socket's blocking mode) by first polling readability with a zero-timeout select().
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(fd, &readSet);
  timeval immediate{};  // {0, 0} => return immediately, never blocks
  const int ready = ::select(0, &readSet, nullptr, nullptr, &immediate);
  if (ready == 0) {
    return false;  // nothing readable: the connection is quiet and presumed alive
  }
  if (ready == SOCKET_ERROR) {
    return true;  // cannot probe the socket: treat it as unusable
  }
  // Readable: peek (now guaranteed not to block) to classify what is pending.
  const int peeked = ::recv(fd, &probe, 1, MSG_PEEK);
  if (peeked == SOCKET_ERROR) {
    // Raced away between select() and recv(): WSAEWOULDBLOCK means the peer is quiet (still alive); any
    // other error (connection reset / aborted) means the connection is dead.
    return ::WSAGetLastError() != WSAEWOULDBLOCK;
  }
  return true;  // EOF (peeked == 0) or unexpected pending bytes (peeked > 0): discard the connection
#else
  for (;;) {
    const ssize_t peeked = ::recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    if (peeked < 0) {
      if (errno == EINTR) {
        continue;
      }
      // EAGAIN/EWOULDBLOCK => no pending data, the connection is quiet and presumed alive; any other
      // errno (ECONNRESET, ...) => the connection is dead.
      return errno != EAGAIN && errno != EWOULDBLOCK;
    }
    return true;  // EOF (peeked == 0) or unexpected pending bytes (peeked > 0): discard the connection
  }
#endif
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

uint8_t FormatAddress(const sockaddr_storage& addr, char* buf, uint8_t bufLen) noexcept {
  assert(bufLen > 1U);
  const char* result = nullptr;
  if (addr.ss_family == AF_INET) {
    const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
    result = ::inet_ntop(AF_INET, &in->sin_addr, buf, static_cast<socklen_t>(bufLen));
  } else if (addr.ss_family == AF_INET6) {
    const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
    result = ::inet_ntop(AF_INET6, &in6->sin6_addr, buf, static_cast<socklen_t>(bufLen));
  }
  if (result == nullptr) {
    buf[0] = '-';
    buf[1] = '\0';
    return 1;
  }
  // Find length of the formatted string
  const char* pEnd = static_cast<const char*>(std::memchr(buf, '\0', bufLen));
  assert(pEnd != nullptr);  // Should never happen since we passed bufLen to inet_ntop
  return static_cast<uint8_t>(pEnd - buf);
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

int64_t ReadOffset(NativeHandle fd, void* buffer, std::size_t len, std::size_t offset) noexcept {
#ifdef AERONET_POSIX
  return static_cast<int64_t>(::pread(fd, buffer, len, static_cast<off_t>(offset)));
#elifdef AERONET_WINDOWS
  // Windows: use _pread equivalent via ReadFile with OVERLAPPED
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(static_cast<int64_t>(offset) & 0xFFFFFFFF);
  ov.OffsetHigh = static_cast<DWORD>((static_cast<int64_t>(offset) >> 32) & 0xFFFFFFFF);
  DWORD bytesRead = 0;
  if (::ReadFile(reinterpret_cast<HANDLE>(_get_osfhandle(static_cast<int>(fd))), buffer, static_cast<DWORD>(len),
                 &bytesRead, &ov)) {
    return static_cast<int64_t>(bytesRead);
  }
  return -1;
#endif
}

#ifdef AERONET_WINDOWS
void CreateLocalSocketPair(NativeHandle& readFd, NativeHandle& writeFd) {
  EnsureWinsockInitialized();

  SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    throw std::runtime_error("CreateLocalSocketPair: socket() failed for listener");
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;

  if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
      ::listen(listener, 1) == SOCKET_ERROR) {
    ::closesocket(listener);
    throw std::runtime_error("CreateLocalSocketPair: bind/listen failed");
  }

  socklen_t addrLen = sizeof(addr);
  ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen);

  SOCKET connector = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (connector == INVALID_SOCKET) {
    ::closesocket(listener);
    throw std::runtime_error("CreateLocalSocketPair: socket() failed for connector");
  }

  if (::connect(connector, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    ::closesocket(connector);
    ::closesocket(listener);
    throw std::runtime_error("CreateLocalSocketPair: connect failed");
  }

  SOCKET accepted = ::accept(listener, nullptr, nullptr);
  ::closesocket(listener);

  if (accepted == INVALID_SOCKET) {
    ::closesocket(connector);
    throw std::runtime_error("CreateLocalSocketPair: accept failed");
  }

  SetNonBlocking(accepted);
  SetNonBlocking(connector);

  SetTcpNoDelay(accepted);
  SetTcpNoDelay(connector);

  readFd = accepted;
  writeFd = connector;
}
#endif

}  // namespace aeronet
