#include "aeronet/event-loop.hpp"

#include <sys/epoll.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
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

static_assert(std::is_trivially_copyable_v<epoll_event> && std::is_standard_layout_v<epoll_event>,
              "epoll_event must be trivially copyable for malloc / realloc usage");
static_assert(std::is_trivially_copyable_v<EventLoop::EventFd> && std::is_standard_layout_v<EventLoop::EventFd>,
              "EventLoop::EventFd must be trivially copyable for malloc / realloc usage");
static_assert(sizeof(epoll_event) >= sizeof(EventLoop::EventFd),
              "EventLoop requires epoll_event to be at least as large as EventFd for the convert loop");

static_assert(EventIn == EPOLLIN, "EventIn value mismatch");
static_assert(EventOut == EPOLLOUT, "EventOut value mismatch");
static_assert(EventErr == EPOLLERR, "EventErr value mismatch");
static_assert(EventHup == EPOLLHUP, "EventHup value mismatch");
static_assert(EventRdHup == EPOLLRDHUP, "EventRdHup value mismatch");
static_assert(EventEt == EPOLLET, "EventEt value mismatch");

}  // namespace

EventLoop::EventLoop(SysDuration pollTimeout, int epollFlags, uint32_t initialCapacity)
    : _nbAllocatedEvents(std::max(1U, initialCapacity)),
      _pollTimeoutMs(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(pollTimeout).count())),
      _baseFd(::epoll_create1(epollFlags)),
      _pEvents(std::malloc(static_cast<std::size_t>(_nbAllocatedEvents) * sizeof(epoll_event))) {
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
  if (this != &rhs) [[likely]] {
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
  if (!add(event)) [[unlikely]] {
    throw_errno("epoll_ctl ADD failed (fd # {}, events=0x{:x})", event.fd, event.eventBmp);
  }
}

bool EventLoop::add(EventFd event) const {
  epoll_event ev{event.eventBmp, epoll_data_t{.fd = event.fd}};
  if (::epoll_ctl(_baseFd.fd(), EPOLL_CTL_ADD, event.fd, &ev) != 0) [[unlikely]] {
    const auto err = errno;
    log::error("epoll_ctl ADD failed (fd # {}, events=0x{:x}, errno={}, msg={})", event.fd, event.eventBmp, err,
               std::strerror(err));
    return false;
  }
  return true;
}

bool EventLoop::mod(EventFd event) const {
  epoll_event ev{event.eventBmp, epoll_data_t{.fd = event.fd}};
  if (::epoll_ctl(_baseFd.fd(), EPOLL_CTL_MOD, event.fd, &ev) != 0) [[unlikely]] {
    const auto err = errno;
    // EBADF or ENOENT can occur during races where a connection is concurrently closed; downgrade severity.
    if (err == EBADF || err == ENOENT) {
      log::warn("epoll_ctl MOD benign failure (fd # {}, events=0x{:x}, errno={}, msg={})", event.fd, event.eventBmp,
                err, std::strerror(err));
    } else {
      log::error("epoll_ctl MOD failed (fd # {}, events=0x{:x}, errno={}, msg={})", event.fd, event.eventBmp, err,
                 std::strerror(err));
    }
    return false;
  }
  return true;
}

void EventLoop::del(int fd) const {
  if (::epoll_ctl(_baseFd.fd(), EPOLL_CTL_DEL, fd, nullptr) != 0) [[unlikely]] {
    // DEL failures are usually benign if fd already closed; log at debug to avoid noise.
    const auto err = errno;
    log::debug("epoll_ctl DEL failed (fd # {}, errno={}, msg={})", fd, err, strerror(err));
  }
}

std::span<const EventLoop::EventFd> EventLoop::poll() {
  const uint32_t capacityBeforePoll = _nbAllocatedEvents;
  auto* epollEvents = static_cast<epoll_event*>(_pEvents);

  const int nbReadyFds = ::epoll_wait(_baseFd.fd(), epollEvents, static_cast<int>(capacityBeforePoll), _pollTimeoutMs);

  if (nbReadyFds == -1) {
    if (errno == EINTR) {
      // Interrupted; treat as no events. Return an empty span with a valid data pointer.
      return {std::launder(reinterpret_cast<EventFd*>(_pEvents)), 0U};
    }
    const auto err = errno;
    log::error("epoll_wait failed (timeout_ms={}, errno={}, msg={})", _pollTimeoutMs, err, std::strerror(err));
    return {};  // data() == nullptr
  }

  // If saturated, grow buffer for subsequent polls.
  if (std::cmp_equal(nbReadyFds, capacityBeforePoll)) {
    const uint32_t newCapacity = capacityBeforePoll * 2U;
    void* newEvents = std::realloc(_pEvents, static_cast<std::size_t>(newCapacity) * sizeof(epoll_event));
    if (newEvents == nullptr) {
      log::error("Failed to reallocate memory for saturated events, keeping actual size of {}", _nbAllocatedEvents);
    } else {
      _pEvents = newEvents;
      _nbAllocatedEvents = newCapacity;
    }
  }

  // Convert epoll_event[] into EventFd[] in-place (EventFd is smaller or equal in size/alignment).
  EventFd* out = std::launder(reinterpret_cast<EventFd*>(_pEvents));
  if constexpr (offsetof(epoll_event, data.fd) != offsetof(EventFd, fd) ||
                offsetof(epoll_event, events) != offsetof(EventFd, eventBmp) ||
                sizeof(epoll_event) != sizeof(EventFd)) {
    epollEvents = static_cast<epoll_event*>(_pEvents);
    for (int idx = 0; idx < nbReadyFds; ++idx) {
      out[idx] = EventFd{epollEvents[idx].data.fd, static_cast<EventBmp>(epollEvents[idx].events)};
    }
  }

  return {out, static_cast<std::size_t>(nbReadyFds)};
}

void EventLoop::updatePollTimeout(SysDuration pollTimeout) {
  _pollTimeoutMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(pollTimeout).count());
}

}  // namespace aeronet
