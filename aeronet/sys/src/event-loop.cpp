#include "aeronet/event-loop.hpp"

#include <sys/epoll.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "aeronet/base-fd.hpp"
#include "aeronet/errno-throw.hpp"
#include "aeronet/event.hpp"
#include "aeronet/log.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

namespace {

static_assert(std::is_trivially_copyable_v<epoll_event>,
              "epoll_event must be trivially copyable for malloc / realloc usage");

static_assert(EventIn == EPOLLIN, "EventIn value mismatch");
static_assert(EventOut == EPOLLOUT, "EventOut value mismatch");
static_assert(EventEt == EPOLLET, "EventEt value mismatch");

}  // namespace

EventLoop::EventLoop(SysDuration pollTimeout, int epollFlags, uint32_t initialCapacity)
    : _nbAllocatedEvents(std::max(1U, initialCapacity)),
      _pollTimeoutMs(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(pollTimeout).count())),
      _baseFd(::epoll_create1(epollFlags)),
      _pEvents(std::malloc(_nbAllocatedEvents * sizeof(epoll_event))) {
  if (_pEvents == nullptr) {
    throw std::bad_alloc();
  }
  if (!_baseFd) {
    auto err = errno;
    log::error("epoll_create1 failed (flags={}, errno={}, msg={})", epollFlags, err, std::strerror(err));
    std::free(_pEvents);
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
      _pEvents(std::exchange(rhs._pEvents, nullptr)) {}

EventLoop& EventLoop::operator=(EventLoop&& rhs) noexcept {
  if (this != &rhs) {
    std::free(_pEvents);

    _nbAllocatedEvents = std::exchange(rhs._nbAllocatedEvents, 0);
    _pollTimeoutMs = rhs._pollTimeoutMs;
    _baseFd = std::move(rhs._baseFd);
    _pEvents = std::exchange(rhs._pEvents, nullptr);
  }
  return *this;
}

EventLoop::~EventLoop() { std::free(_pEvents); }

void EventLoop::addOrThrow(EventFd event) const {
  if (!add(event)) {
    throw_errno("epoll_ctl ADD failed (fd # {}, events=0x{:x})", event.fd, event.eventBmp);
  }
}

bool EventLoop::add(EventFd event) const {
  epoll_event ev{event.eventBmp, epoll_data_t{.fd = event.fd}};
  if (::epoll_ctl(_baseFd.fd(), EPOLL_CTL_ADD, event.fd, &ev) != 0) {
    auto err = errno;
    log::error("epoll_ctl ADD failed (fd # {}, events=0x{:x}, errno={}, msg={})", event.fd, event.eventBmp, err,
               std::strerror(err));
    return false;
  }
  return true;
}

bool EventLoop::mod(EventFd event) const {
  epoll_event ev{event.eventBmp, epoll_data_t{.fd = event.fd}};
  if (::epoll_ctl(_baseFd.fd(), EPOLL_CTL_MOD, event.fd, &ev) != 0) {
    auto err = errno;
    log::error("epoll_ctl MOD failed (fd # {}, events=0x{:x}, errno={}, msg={})", event.fd, event.eventBmp, err,
               std::strerror(err));
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

int EventLoop::poll(const std::function<void(EventFd)>& cb) {
  const int nbReadyFds = ::epoll_wait(_baseFd.fd(), static_cast<epoll_event*>(_pEvents),
                                      static_cast<int>(_nbAllocatedEvents), _pollTimeoutMs);
  if (nbReadyFds < 0) {
    if (errno == EINTR) {
      return 0;  // interrupted; treat as no events
    }
    auto err = errno;
    log::error("epoll_wait failed (timeout_ms={}, errno={}, msg={})", _pollTimeoutMs, err, std::strerror(err));
    return -1;
  }
  for (int fdPos = 0; fdPos < nbReadyFds; ++fdPos) {
    const epoll_event& event = static_cast<epoll_event*>(_pEvents)[static_cast<uint32_t>(fdPos)];

    cb(EventFd{event.data.fd, event.events});
  }
  if (std::cmp_equal(nbReadyFds, _nbAllocatedEvents)) {
    // Saturated buffer: grow exponentially (amortized O(1) realloc). No shrink to avoid churn.
    auto* newEvents = std::realloc(_pEvents, sizeof(epoll_event) * 2UL * _nbAllocatedEvents);
    if (newEvents == nullptr) {
      log::error("Failed to reallocate memory for saturated events, keeping actual size of {}", _nbAllocatedEvents);
    } else {
      _pEvents = newEvents;
      _nbAllocatedEvents *= 2UL;
    }
  }
  return nbReadyFds;
}

void EventLoop::updatePollTimeout(SysDuration pollTimeout) {
  _pollTimeoutMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(pollTimeout).count());
}

}  // namespace aeronet
