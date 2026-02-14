#include "aeronet/unix-socket.hpp"

#include <sys/socket.h>
#include <sys/un.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>

#include "aeronet/base-fd.hpp"
#include "aeronet/errno-throw.hpp"

#ifdef __linux__
// Linux supports SOCK_NONBLOCK | SOCK_CLOEXEC atomically at socket creation.
#else
#include "aeronet/socket-ops.hpp"  // SetNonBlocking, SetCloseOnExec
#endif

namespace aeronet {

namespace {
constexpr int ToNativeType(UnixSocket::Type type) {
  switch (type) {
    case UnixSocket::Type::Datagram:
      return SOCK_DGRAM;
    case UnixSocket::Type::Stream:
      return SOCK_STREAM;
    default:
      throw std::invalid_argument("Invalid UnixSocket::Type");
  }
}
}  // namespace

UnixSocket::UnixSocket(Type type) {
  const int nativeType = ToNativeType(type);
#ifdef __linux__
  _baseFd = BaseFd(::socket(AF_UNIX, nativeType | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
#else
  _baseFd = BaseFd(::socket(AF_UNIX, nativeType, 0));
#endif
  if (!_baseFd) {
    throw_errno("UnixSocket: socket creation failed");
  }
#ifndef __linux__
  // On macOS / others: set non-blocking and close-on-exec via fcntl.
  if (!SetNonBlocking(_baseFd.fd()) || !SetCloseOnExec(_baseFd.fd())) {
    throw_errno("UnixSocket: fcntl failed");
  }
#endif
}

int UnixSocket::connect(std::string_view path) noexcept {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.data(), path.size());
  addr.sun_path[path.size()] = '\0';
  const auto addrlen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
  return ::connect(_baseFd.fd(), reinterpret_cast<sockaddr*>(&addr), addrlen);
}

int64_t UnixSocket::send(const void* data, std::size_t len) noexcept {
#ifdef __linux__
  return static_cast<int64_t>(::send(_baseFd.fd(), data, len, MSG_DONTWAIT | MSG_NOSIGNAL));
#elifdef __APPLE__
  // macOS: MSG_DONTWAIT is supported, MSG_NOSIGNAL is not — use SO_NOSIGPIPE on the socket instead.
  return ::send(_baseFd.fd(), data, len, MSG_DONTWAIT);
#else
  return ::send(_baseFd.fd(), data, len, MSG_DONTWAIT);
#endif
}

}  // namespace aeronet
