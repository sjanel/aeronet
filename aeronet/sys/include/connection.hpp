#pragma once

#include "socket.hpp"

namespace aeronet {

// Simple RAII class wrapping a socket file descriptor.
class Connection : public BaseFd {
 public:
  explicit Connection(const Socket &socket);
};

}  // namespace aeronet