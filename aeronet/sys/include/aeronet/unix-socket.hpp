#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/base-fd.hpp"
#include "aeronet/native-handle.hpp"

#if defined(AERONET_LINUX) || defined(AERONET_MACOS)
#include <sys/un.h>
#elif defined(AERONET_WINDOWS)
#include <afunix.h>
#endif

namespace aeronet {

// Maximum length of a Unix domain socket path (platform-dependent, typically 104–108 bytes).
inline constexpr std::size_t kUnixSocketMaxPath =
#if defined(AERONET_LINUX) || defined(AERONET_MACOS)
    sizeof(sockaddr_un{}.sun_path)
#elif defined(AERONET_WINDOWS)
    sizeof(SOCKADDR_UN{}.sun_path)
#else
#error "UnixSocket is only supported on Linux, macOS, and Windows"
#endif
    ;

// RAII wrapper for a Unix-domain socket with platform-portable
// creation (handles SOCK_NONBLOCK / SOCK_CLOEXEC on Linux vs fcntl on macOS).
class UnixSocket {
 public:
  // Socket type for Unix domain sockets.
  enum class Type : std::uint8_t { Datagram, Stream };

  UnixSocket() noexcept = default;

  // Create a non-blocking, close-on-exec AF_UNIX socket of the given type.
  // Throws on failure.
  explicit UnixSocket(Type type);

  [[nodiscard]] NativeHandle fd() const noexcept { return _baseFd.fd(); }

  explicit operator bool() const noexcept { return static_cast<bool>(_baseFd); }

  // Connect to a Unix-domain socket at `path`.
  // Returns 0 on success, -1 on error (errno is set).
  int connect(std::string_view path) noexcept;

  // Non-blocking send with suppressed SIGPIPE.
  // Returns the number of bytes sent, or -1 on error (errno is set).
  int64_t send(const void* data, std::size_t len) noexcept;

 private:
  BaseFd _baseFd;
};

}  // namespace aeronet
