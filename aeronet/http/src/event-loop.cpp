#include "event-loop.hpp"

#include <sys/epoll.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>

#include "base-fd.hpp"
#include "invalid_argument_exception.hpp"
#include "log.hpp"
#include "timedef.hpp"

namespace aeronet {

EventLoop::EventLoop(int epollFlags, std::size_t initialCapacity) : BaseFd(epoll_create1(epollFlags)) {
  if (!this->isOpened()) {
    auto err = errno;
    log::error("epoll_create1 failed (flags={}, errno={}, msg={})", epollFlags, err, std::strerror(err));
    throw invalid_argument("epoll_create1 failed");
  }
  if (initialCapacity == 0) {
    log::warn("EventLoop constructed with initialCapacity=0; promoting to 1");
    initialCapacity = 1;
  }
  _events.resize(static_cast<vector<epoll_event>::size_type>(initialCapacity));
}

bool EventLoop::add(int fd, uint32_t events) const {
  epoll_event ev{events, epoll_data_t{.fd = fd}};
  if (epoll_ctl(this->fd(), EPOLL_CTL_ADD, fd, &ev) != 0) {
    auto err = errno;
    log::error("epoll_ctl ADD failed (fd={}, events=0x{:x}, errno={}, msg={})", fd, events, err, std::strerror(err));
    return false;
  }
  return true;
}

bool EventLoop::mod(int fd, uint32_t events) const {
  epoll_event ev{events, epoll_data_t{.fd = fd}};
  if (epoll_ctl(this->fd(), EPOLL_CTL_MOD, fd, &ev) != 0) {
    auto err = errno;
    log::error("epoll_ctl MOD failed (fd={}, events=0x{:x}, errno={}, msg={})", fd, events, err, std::strerror(err));
    return false;
  }
  return true;
}
void EventLoop::del(int fd) const {
  if (epoll_ctl(this->fd(), EPOLL_CTL_DEL, fd, nullptr) != 0) {
    // DEL failures are usually benign if fd already closed; log at debug to avoid noise.
    auto err = errno;
    log::debug("epoll_ctl DEL failed (fd={}, errno={}, msg={})", fd, err, strerror(err));
  }
}

int EventLoop::poll(Duration timeout, const std::function<void(int, uint32_t)>& cb) {
  const auto timeoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
  int timeoutEpollWait;
  if (std::cmp_less(std::numeric_limits<int>::max(), timeoutMs)) {
    log::warn("Timeout value is too large, clamping to max int");
    timeoutEpollWait = std::numeric_limits<int>::max();
  } else {
    timeoutEpollWait = static_cast<int>(timeoutMs);
  }
  int nbReadyFds = epoll_wait(this->fd(), _events.data(), static_cast<int>(_events.size()), timeoutEpollWait);
  if (nbReadyFds < 0) {
    if (errno == EINTR) {
      _lastReadyCount = 0;
      return 0;  // interrupted; treat as no events
    }
    auto err = errno;
    log::error("epoll_wait failed (timeout_ms={}, errno={}, msg={})", timeoutEpollWait, err, std::strerror(err));
    _lastReadyCount = 0;
    return -1;
  }
  _lastReadyCount = nbReadyFds;
  for (int fdPos = 0; fdPos < nbReadyFds; ++fdPos) {
    const auto& event = _events[static_cast<uint32_t>(fdPos)];

    cb(event.data.fd, event.events);
  }
  if (std::cmp_equal(nbReadyFds, _events.size())) {
    // Saturated buffer: grow exponentially (amortized O(1) realloc). No shrink to avoid churn.
    _events.resize(_events.size() * 2);
  }
  return nbReadyFds;
}

}  // namespace aeronet
