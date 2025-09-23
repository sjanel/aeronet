#pragma once

#include <cstdint>

#include "base-fd.hpp"

namespace aeronet {

// Simple RAII class wrapping a socket file descriptor.
class Socket : public BaseFd {
 public:
  enum class Type : int8_t { STREAM, DATAGRAM };

  Socket() noexcept = default;

  explicit Socket(Type type, int protocol = 0);
};

}  // namespace aeronet