#pragma once

#include <cstdint>

#include "aeronet/base-fd.hpp"
#include "aeronet/platform.hpp"

namespace aeronet {

// Simple RAII class wrapping a blocking socket file descriptor.
class Socket {
 public:
  enum class Type : std::uint8_t { Stream, StreamNonBlock };

  Socket() noexcept = default;

  // Construct a socket with the given type and protocol.
  // Throws std::system_error on failure.
  explicit Socket(Type type, int protocol = 0);

  [[nodiscard]] NativeHandle fd() const noexcept { return _baseFd.fd(); }

  explicit operator bool() const noexcept { return static_cast<bool>(_baseFd); }

  // Try to bind the socket to the given port with specified options.
  // Returns true on success, false on failure.
  // Throws std::system_error on setsockopt failure.
  [[nodiscard]] bool tryBind(bool reusePort, bool tcpNoDelay, uint16_t port) const;

  // Bind and start listening on the given port. If port is 0, an ephemeral port is chosen and updated in the argument.
  // Throws std::system_error on failure.
  void bindAndListen(bool reusePort, bool tcpNoDelay, uint16_t& port);

  void close() noexcept { _baseFd.close(); }

 private:
  BaseFd _baseFd;
};

}  // namespace aeronet