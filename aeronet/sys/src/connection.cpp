
#include "aeronet/connection.hpp"

#ifdef AERONET_WINDOWS
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

#include "aeronet/base-fd.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/socket-ops.hpp"  // SetNonBlocking, SetCloseOnExec
#include "aeronet/socket.hpp"
#include "aeronet/system-error-message.hpp"
#include "aeronet/system-error.hpp"

namespace aeronet {

namespace {
NativeHandle ComputeConnectionFd(NativeHandle socketFd, sockaddr_storage& peerAddress) {
  socklen_t in_len = sizeof(peerAddress);

  peerAddress = {};

#ifdef AERONET_LINUX
  const NativeHandle fd =
      ::accept4(socketFd, reinterpret_cast<sockaddr*>(&peerAddress), &in_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
  const NativeHandle fd = ::accept(socketFd, reinterpret_cast<sockaddr*>(&peerAddress), &in_len);
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

#ifndef AERONET_LINUX
  // On non-Linux POSIX, set non-blocking + close-on-exec + no-sigpipe after accept.
  SetNonBlocking(fd);
#ifdef AERONET_POSIX
  SetCloseOnExec(fd);
  SetNoSigPipe(fd);
#endif
#endif

  if (log::get_level() <= log::level::debug) {
    char peerAddressBuffer[kFormattedAddressCapacity];
    const auto peerAddressLength =
        FormatAddress(peerAddress, peerAddressBuffer, static_cast<uint8_t>(sizeof(peerAddressBuffer)));
    log::debug("Connection fd # {} opened (peer={})", static_cast<intptr_t>(fd),
               std::string_view(peerAddressBuffer, peerAddressLength));
  }
  return fd;
}

}  // namespace

Connection::Connection(const Socket& socket, sockaddr_storage& peerAddress)
    : _baseFd(ComputeConnectionFd(socket.fd(), peerAddress)) {}

Connection::Connection(BaseFd&& bd) noexcept : _baseFd(std::move(bd)) {}

}  // namespace aeronet
