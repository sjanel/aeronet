
#include "aeronet/connection.hpp"

#include "aeronet/platform.hpp"

#ifdef AERONET_WINDOWS
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <cstring>
#include <utility>

#include "aeronet/base-fd.hpp"
#include "aeronet/log.hpp"
#include "aeronet/socket.hpp"

#ifndef AERONET_LINUX
#include "aeronet/socket-ops.hpp"  // SetNonBlocking, SetCloseOnExec
#endif

namespace aeronet {

#ifdef AERONET_POSIX
static_assert(EAGAIN == EWOULDBLOCK, "Add handling for EWOULDBLOCK if different from EAGAIN");
#endif

namespace {
NativeHandle ComputeConnectionFd(NativeHandle socketFd) {
  sockaddr_in in_addr{};
  socklen_t in_len = sizeof(in_addr);

#ifdef AERONET_LINUX
  NativeHandle fd = ::accept4(socketFd, reinterpret_cast<sockaddr*>(&in_addr), &in_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
  NativeHandle fd = ::accept(socketFd, reinterpret_cast<sockaddr*>(&in_addr), &in_len);
#endif

  if (fd == kInvalidHandle) [[unlikely]] {
    const auto savedErr = LastSystemError();
    if (savedErr == error::kWouldBlock) {
      log::trace("Connection accept would block: {} - this is expected if no pending connections",
                 SystemErrorMessage(savedErr));
    } else {
      log::error("Connection accept failed for socket fd # {}: {}", static_cast<intptr_t>(socketFd),
                 SystemErrorMessage(savedErr));
    }
    return kInvalidHandle;
  }

#if !defined(AERONET_LINUX)
  // On non-Linux POSIX, set non-blocking + close-on-exec + no-sigpipe after accept.
  SetNonBlocking(fd);
#ifdef AERONET_POSIX
  SetCloseOnExec(fd);
  SetNoSigPipe(fd);
#endif
#endif

  log::debug("Connection fd # {} opened", static_cast<intptr_t>(fd));
  return fd;
}

}  // namespace

Connection::Connection(const Socket& socket) : _baseFd(ComputeConnectionFd(socket.fd())) {}

Connection::Connection(BaseFd&& bd) noexcept : _baseFd(std::move(bd)) {}

}  // namespace aeronet
