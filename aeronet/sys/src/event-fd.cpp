#include "aeronet/event-fd.hpp"

#include <sys/eventfd.h>

#include <cerrno>
#include <cstring>

#include "aeronet/base-fd.hpp"
#include "aeronet/errno-throw.hpp"
#include "aeronet/log.hpp"

namespace aeronet {

EventFd::EventFd() : _baseFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
  if (fd() == -1) {
    throw_errno("Unable to create a new EventFd");
  }
  log::debug("EventFd fd # {} opened", fd());
}

void EventFd::send() const noexcept {
  static constexpr eventfd_t one = 1;
  const auto ret = ::eventfd_write(fd(), one);
  if (ret == -1) {
    auto savedErr = errno;
    if (savedErr != EAGAIN) {
      log::error("Event fd send failed err={}: {}", savedErr, std::strerror(savedErr));
    }
  } else {
    log::trace("Event fd send succeeded");
  }
}

void EventFd::read() const noexcept {
  eventfd_t counterValue;
  const auto ret = ::eventfd_read(fd(), &counterValue);
  if (ret == -1) {
    auto savedErr = errno;
    if (savedErr != EAGAIN) {
      log::error("Event fd read failed err={}: {}", savedErr, std::strerror(savedErr));
    }
  } else {
    log::trace("Event fd drained (value={})", static_cast<unsigned long long>(counterValue));
  }
}

}  // namespace aeronet