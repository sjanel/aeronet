#include "aeronet/unix-dogstatsd-sink.hpp"

#include "aeronet/system-error-message.hpp"
#include "aeronet/system-error.hpp"

#ifdef AERONET_POSIX
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#elifdef AERONET_WINDOWS
#include <afunix.h>
#include <process.h>
#include <ws2tcpip.h>
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include "aeronet/unix-socket.hpp"

namespace aeronet::test {

namespace {
std::string MakeUniquePath() {
  static std::atomic<uint64_t> counter{0};
#ifdef AERONET_POSIX
  const auto pid = static_cast<unsigned long>(::getpid());
#else
  const auto pid = static_cast<unsigned long>(_getpid());
#endif
  const auto suffix = counter.fetch_add(1, std::memory_order_relaxed);
  return std::string("/tmp/aeronet-dogstatsd-") + std::to_string(pid) + "-" + std::to_string(suffix);
}
}  // namespace

UnixDogstatsdSink::UnixDogstatsdSink() : _fd(UnixSocket::Type::Datagram) {
  _path = MakeUniquePath();
#ifdef AERONET_POSIX
  ::unlink(_path.c_str());
#else
  _unlink(_path.c_str());
#endif

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, _path.c_str(), sizeof(addr.sun_path) - 1);
  const socklen_t addrlen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + _path.size() + 1);
  if (::bind(_fd.fd(), reinterpret_cast<sockaddr*>(&addr), addrlen) != 0) {
    auto err = LastSystemError();
    throw std::runtime_error(std::string("bind failed: ") + SystemErrorMessage(err));
  }
}

UnixDogstatsdSink& UnixDogstatsdSink::operator=(UnixDogstatsdSink&& other) noexcept {
  if (this != &other) {
    closeAndUnlink();
    _fd = std::move(other._fd);
    _path = std::move(other._path);
    other._path.clear();
  }
  return *this;
}

std::string UnixDogstatsdSink::recvMessage(int timeoutMs) const {
  if (!_fd) {
    return {};
  }
  pollfd pfd{};  // NOLINT(misc-include-cleaner) poll.h should be the correct header
  pfd.fd = _fd.fd();
  pfd.events = POLLIN;  // NOLINT(misc-include-cleaner) poll.h should be the correct header
#ifdef AERONET_WINDOWS
  const int ready = ::WSAPoll(&pfd, 1, timeoutMs);
#else
  const int ready = ::poll(&pfd, 1, timeoutMs);  // NOLINT(misc-include-cleaner) poll.h should be the correct header
#endif
  if (ready <= 0 || (pfd.revents & POLLIN) == 0) {
    return {};
  }
  std::array<char, 512> buf;
#ifdef AERONET_WINDOWS
  const auto bytes = ::recv(_fd.fd(), buf.data(), static_cast<int>(buf.size()), 0);
#else
  const auto bytes = ::recv(_fd.fd(), buf.data(), buf.size(), 0);
#endif
  if (bytes <= 0) {
    return {};
  }
  return {buf.data(), static_cast<std::size_t>(bytes)};
}

void UnixDogstatsdSink::closeAndUnlink() {
  if (!_path.empty()) {
#ifdef AERONET_POSIX
    ::unlink(_path.c_str());
#else
    _unlink(_path.c_str());
#endif
    _path.clear();
  }
}

}  // namespace aeronet::test