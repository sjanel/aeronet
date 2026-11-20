#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>

#ifdef AERONET_POSIX
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#elifdef AERONET_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "aeronet/close-native-handle.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/socket-ops.hpp"

// Raw loopback servers for the client tests: they script the bytes a server sends, byte for byte.
namespace aeronet::test {

// Wait until `fd` is readable (or timeoutMs elapses); poll on POSIX, select on Windows.
inline bool WaitReadable(NativeHandle fd, int timeoutMs) {
#ifdef AERONET_WINDOWS
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(fd, &readSet);
  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  return ::select(0, &readSet, nullptr, nullptr, &tv) > 0;
#else
  pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};  // NOLINT(misc-include-cleaner)
  return ::poll(&pfd, 1, timeoutMs) > 0;                 // NOLINT(misc-include-cleaner)
#endif
}

// Connects to the loopback listening socket `listenFd`, waking up a thread waiting for it to be readable, so that it
// notices its stop request right away instead of at its next poll timeout. Returns the connected socket, to close once
// that thread stopped (kInvalidHandle on failure: the thread then only stops at its next poll timeout).
inline NativeHandle ConnectToListener(NativeHandle listenFd) {
  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);  // NOLINT(misc-include-cleaner)
  if (::getsockname(listenFd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    return kInvalidHandle;
  }
  const NativeHandle fd = ::socket(addr.ss_family, SOCK_STREAM, 0);
  if (fd != kInvalidHandle && ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), len) != 0) {
    CloseNativeHandle(fd);
    return kInvalidHandle;
  }
  return fd;
}

// Stops `thread`, which accepts the connections of `listenFd` until `stop` is set and checks it after each accept.
inline void StopAcceptThread(std::thread& thread, NativeHandle listenFd, std::atomic<bool>& stop) {
  stop.store(true, std::memory_order_relaxed);
  if (!thread.joinable()) {
    return;
  }
  const NativeHandle wakeFd = ConnectToListener(listenFd);
  thread.join();
  if (wakeFd != kInvalidHandle) {
    CloseNativeHandle(wakeFd);
  }
}

// Minimal scriptable raw TCP server bound to an ephemeral loopback port. For each accepted connection
// it invokes a user handler with the socket fd and a 0-based connection index, then closes the socket.
class RawServer {
 public:
  using Handler = std::function<void(NativeHandle fd, int connIndex)>;

  // family is AF_INET (default) or AF_INET6. The IPv6 path fails softly (listening() stays false) so a
  // test can GTEST_SKIP() on hosts/CI without IPv6 loopback instead of hard-failing.
  explicit RawServer(Handler handler, int family = AF_INET) : _handler(std::move(handler)) {
#ifdef AERONET_WINDOWS
    EnsureWinsockInitialized();
#endif
    if (family == AF_INET6) {
      setupIpv6();
    } else {
      setupIpv4();
    }
    if (_listenFd != kInvalidHandle) {
      _thread = std::thread([this] { run(); });
    }
  }

  RawServer(const RawServer&) = delete;
  RawServer& operator=(const RawServer&) = delete;
  RawServer(RawServer&&) = delete;
  RawServer& operator=(RawServer&&) = delete;

  ~RawServer() {
    StopAcceptThread(_thread, _listenFd, _stop);
    if (_listenFd != kInvalidHandle) {
      CloseNativeHandle(_listenFd);
    }
  }

  [[nodiscard]] uint16_t port() const noexcept { return _port; }
  [[nodiscard]] bool listening() const noexcept { return _listenFd != kInvalidHandle; }

 private:
  void setupIpv4() {
    _listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_NE(_listenFd, kInvalidHandle);
    int one = 1;
    // NOLINTNEXTLINE(misc-include-cleaner)
    ::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    EXPECT_EQ(::bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t len = sizeof(addr);  // NOLINT(misc-include-cleaner)
    EXPECT_EQ(::getsockname(_listenFd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    _port = ntohs(addr.sin_port);
    EXPECT_EQ(::listen(_listenFd, 8), 0);
  }

  // Soft setup on IPv6 loopback: any failure leaves _listenFd == kInvalidHandle so the test skips.
  void setupIpv6() {
    _listenFd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (_listenFd == kInvalidHandle) {
      return;
    }
    int one = 1;
    // NOLINTNEXTLINE(misc-include-cleaner)
    ::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_loopback;
    addr.sin6_port = 0;
    if (::bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      CloseNativeHandle(_listenFd);
      _listenFd = kInvalidHandle;
      return;
    }
    socklen_t len = sizeof(addr);  // NOLINT(misc-include-cleaner)
    if (::getsockname(_listenFd, reinterpret_cast<sockaddr*>(&addr), &len) != 0 || ::listen(_listenFd, 8) != 0) {
      CloseNativeHandle(_listenFd);
      _listenFd = kInvalidHandle;
      return;
    }
    _port = ntohs(addr.sin6_port);
  }

  void run() {
    int idx = 0;
    while (!_stop.load(std::memory_order_relaxed)) {
      if (!WaitReadable(_listenFd, 20)) {
        continue;  // timeout (re-check the stop flag) or interrupted
      }
      const NativeHandle fd = ::accept(_listenFd, nullptr, nullptr);
      if (fd == kInvalidHandle) {
        continue;
      }
      if (_stop.load(std::memory_order_relaxed)) {  // the wake-up connection of StopAcceptThread
        CloseNativeHandle(fd);
        break;
      }
      // macOS: a write to a client that gave up (timeout, abort) must fail with EPIPE, not kill the test with SIGPIPE.
      EXPECT_TRUE(SetNoSigPipe(fd));
      _handler(fd, idx++);
      CloseNativeHandle(fd);
    }
  }

  Handler _handler;
  std::thread _thread;
  std::atomic<bool> _stop{false};
  NativeHandle _listenFd{kInvalidHandle};
  uint16_t _port{0};
};

}  // namespace aeronet::test
