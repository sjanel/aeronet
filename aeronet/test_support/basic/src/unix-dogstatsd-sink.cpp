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
  if (ready <= 0 || (pfd.revents & POLLIN) == 0) {  // NOLINT(bugprone-signed-bitwise)
    return {};
  }

  std::string out;

  // Larger than any datagram sent in tests, so that none is truncated.
  static constexpr std::size_t kMaxMessageSize = 1U << 16U;

  out.resize_and_overwrite(kMaxMessageSize,
                           [fd = _fd.fd()](char* pData, [[maybe_unused]] std::size_t newCap) -> std::size_t {
                             const auto bytes = ::recv(fd, pData, kMaxMessageSize, 0);
                             if (bytes < 0) {
                               throw std::runtime_error("UnixDogstatsdSink::recvMessage failed");
                             }
                             return static_cast<std::size_t>(bytes);
                           });

  return out;
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