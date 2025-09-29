#pragma once

#include "base-fd.hpp"

namespace aeronet {

// Simple RAII class wrapping a Event file descriptor
class EventFd : public BaseFd {
 public:
  // Create eventfd for wakeups (non-blocking, close-on-exec)
  EventFd();

  // send an event
  void send();

  // read an event
  void read();
};

}  // namespace aeronet