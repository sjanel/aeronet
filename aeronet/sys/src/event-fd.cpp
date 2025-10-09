#include "event-fd.hpp"

#include <sys/eventfd.h>

#include <cerrno>
#include <cstring>

#include "base-fd.hpp"
#include "exception.hpp"
#include "log.hpp"

namespace aeronet {

EventFd::EventFd() : _baseFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
  if (_baseFd.fd() < 0) {
    throw exception("Unable to create a new eventfd, with error {}", std::strerror(errno));
  }
  log::debug("EventFd fd={} opened", _baseFd.fd());
}

void EventFd::send() const {
  static constexpr eventfd_t one = 1;
  const auto ret = ::eventfd_write(_baseFd.fd(), one);
  if (ret != 0) {
    auto savedErr = errno;
    if (savedErr != EAGAIN) {
      log::error("Wakeup fd send failed err={}: {}", savedErr, std::strerror(savedErr));
    }
  } else {
    log::trace("Wakeup fd send succeeded");
  }
}

void EventFd::read() const {
  eventfd_t counterValue;
  if (::eventfd_read(fd(), &counterValue) == 0) {
    log::trace("Wakeup fd drained (value={})", static_cast<unsigned long long>(counterValue));
  }
}

}  // namespace aeronet