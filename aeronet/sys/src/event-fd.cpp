#include "event-fd.hpp"

#include <sys/eventfd.h>

#include <cerrno>
#include <cstring>

#include "base-fd.hpp"
#include "exception.hpp"
#include "log.hpp"

namespace aeronet {

EventFd::EventFd() : BaseFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
  if (this->fd() < 0) {
    throw exception("Unable to create a new eventfd, with error {}", std::strerror(errno));
  }
}

void EventFd::send() {
  eventfd_t one = 1;
  ::eventfd_write(this->fd(), one);  // ignore failure; best effort
}

void EventFd::read() {
  eventfd_t counterValue;
  if (::eventfd_read(this->fd(), &counterValue) == 0) {
    log::trace("Wakeup fd drained (value={})", static_cast<unsigned long long>(counterValue));
  }
}

}  // namespace aeronet