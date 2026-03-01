#pragma once

#include "aeronet/base-fd.hpp"
#include "aeronet/platform.hpp"

namespace aeronet {

// Simple RAII class wrapping a platform wakeup mechanism.
// Linux  : eventfd (single fd, non-blocking, close-on-exec)
// macOS  : pipe    (read end exposed as fd(), write end internal)
// Windows: manual-reset event (CreateEventW)
class EventFd {
 public:
  // Create the wakeup fd/handle.
  EventFd();

  // Send a wakeup event.
  void send() const noexcept;

  // Drain / read pending wakeup events.
  void read() const noexcept;

  [[nodiscard]] NativeHandle fd() const noexcept { return _baseFd.fd(); }

 private:
  BaseFd _baseFd;
#ifdef AERONET_MACOS
  BaseFd _writeFd;  // write end of the pipe
#endif
};

}  // namespace aeronet