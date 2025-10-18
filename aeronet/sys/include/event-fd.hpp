#pragma once

#include "base-fd.hpp"

namespace aeronet {

// Simple RAII class wrapping a Event file descriptor
class EventFd {
 public:
  // Create eventfd for wakeups (non-blocking, close-on-exec)
  EventFd();

  // send an event
  void send() const noexcept;

  // read an event
  void read() const;

  [[nodiscard]] int fd() const noexcept { return _baseFd.fd(); }

 private:
  BaseFd _baseFd;
};

}  // namespace aeronet