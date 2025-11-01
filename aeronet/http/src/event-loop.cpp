#include "event-loop.hpp"

#include <sys/epoll.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "base-fd.hpp"
#include "errno_throw.hpp"
#include "log.hpp"
#include "timedef.hpp"

namespace aeronet {

namespace {

int ComputeEpollTimeoutMs(SysDuration timeout) {
  const auto timeoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
  if (std::cmp_less(std::numeric_limits<int>::max(), timeoutMs)) {
    log::warn("Timeout value is too large, clamping to max int");
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(timeoutMs);
}

}  // namespace

static_assert(std::is_trivially_copyable_v<epoll_event>);

EventLoop::EventLoop(SysDuration pollTimeout, int epollFlags, uint32_t initialCapacity)
    : _nbAllocatedEvents(std::max(1U, initialCapacity)),
      _pollTimeoutMs(ComputeEpollTimeoutMs(pollTimeout)),
      _baseFd(::epoll_create1(epollFlags)),
      _events(static_cast<epoll_event*>(std::malloc(sizeof(epoll_event) * _nbAllocatedEvents))) {
  if (_events == nullptr) {
    throw std::bad_alloc();
  }
  if (!_baseFd) {
    auto err = errno;
    log::error("epoll_create1 failed (flags={}, errno={}, msg={})", epollFlags, err, std::strerror(err));
    throw std::runtime_error("epoll_create1 failed");
  }
  if (initialCapacity == 0) {
    log::warn("EventLoop constructed with initialCapacity=0; promoting to 1");
  }

  log::debug("EventLoop fd # {} opened", _baseFd.fd());
}

EventLoop::EventLoop(EventLoop&& rhs) noexcept
    : _nbAllocatedEvents(std::exchange(rhs._nbAllocatedEvents, 0)),
      _pollTimeoutMs(rhs._pollTimeoutMs),
      _baseFd(std::move(rhs._baseFd)),
      _events(std::exchange(rhs._events, nullptr)) {}

EventLoop& EventLoop::operator=(EventLoop&& rhs) noexcept {
  if (this != &rhs) {
    std::free(_events);

    _nbAllocatedEvents = std::exchange(rhs._nbAllocatedEvents, 0);
    _pollTimeoutMs = rhs._pollTimeoutMs;
    _baseFd = std::move(rhs._baseFd);
    _events = std::exchange(rhs._events, nullptr);
  }
  return *this;
}

EventLoop::~EventLoop() { std::free(_events); }

void EventLoop::add_or_throw(int fd, uint32_t events) const {
  if (!add(fd, events)) {
    throw_errno("epoll_ctl ADD failed (fd # {}, events=0x{:x}): {}", fd, events);
  }
}

bool EventLoop::add(int fd, uint32_t events) const {
  epoll_event ev{events, epoll_data_t{.fd = fd}};
  if (::epoll_ctl(_baseFd.fd(), EPOLL_CTL_ADD, fd, &ev) != 0) {
    auto err = errno;
    log::error("epoll_ctl ADD failed (fd # {}, events=0x{:x}, errno={}, msg={})", fd, events, err, std::strerror(err));
    return false;
  }
  return true;
}

bool EventLoop::mod(int fd, uint32_t events) const {
  epoll_event ev{events, epoll_data_t{.fd = fd}};
  if (::epoll_ctl(_baseFd.fd(), EPOLL_CTL_MOD, fd, &ev) != 0) {
    auto err = errno;
    log::error("epoll_ctl MOD failed (fd # {}, events=0x{:x}, errno={}, msg={})", fd, events, err, std::strerror(err));
    return false;
  }
  return true;
}

void EventLoop::del(int fd) const {
  if (::epoll_ctl(_baseFd.fd(), EPOLL_CTL_DEL, fd, nullptr) != 0) {
    // DEL failures are usually benign if fd already closed; log at debug to avoid noise.
    auto err = errno;
    log::debug("epoll_ctl DEL failed (fd # {}, errno={}, msg={})", fd, err, strerror(err));
  }
}

int EventLoop::poll(const std::function<void(int, uint32_t)>& cb) {
  const int nbReadyFds = ::epoll_wait(_baseFd.fd(), _events, static_cast<int>(_nbAllocatedEvents), _pollTimeoutMs);
  if (nbReadyFds < 0) {
    if (errno == EINTR) {
      return 0;  // interrupted; treat as no events
    }
    auto err = errno;
    log::error("epoll_wait failed (timeout_ms={}, errno={}, msg={})", _pollTimeoutMs, err, std::strerror(err));
    return -1;
  }
  for (int fdPos = 0; fdPos < nbReadyFds; ++fdPos) {
    const auto& event = _events[static_cast<uint32_t>(fdPos)];

    cb(event.data.fd, event.events);
  }
  if (std::cmp_equal(nbReadyFds, _nbAllocatedEvents)) {
    // Saturated buffer: grow exponentially (amortized O(1) realloc). No shrink to avoid churn.
    auto* newEvents = static_cast<epoll_event*>(std::realloc(_events, sizeof(epoll_event) * 2UL * _nbAllocatedEvents));
    if (newEvents == nullptr) {
      log::error("Failed to reallocate memory for saturated events, keeping actual size of {}", _nbAllocatedEvents);
    } else {
      _events = newEvents;
      _nbAllocatedEvents *= 2UL;
    }
  }
  return nbReadyFds;
}

}  // namespace aeronet
