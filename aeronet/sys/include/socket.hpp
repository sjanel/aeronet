#pragma once

#include "base-fd.hpp"

namespace aeronet {

// Simple RAII class wrapping a blocking socket file descriptor.
class Socket {
 public:
  Socket() noexcept = default;

  Socket(int type, int protocol = 0);

  [[nodiscard]] int fd() const noexcept { return _baseFd.fd(); }

  void close() noexcept { _baseFd.close(); }

 private:
  BaseFd _baseFd;
};

}  // namespace aeronet