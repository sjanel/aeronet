#include "aeronet/event-loop.hpp"

#ifdef AERONET_LINUX
#include <sys/epoll.h>
#elifdef AERONET_MACOS
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#elifdef AERONET_WINDOWS
// WSAPoll headers included via winsock2.h
#endif

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "aeronet/base-fd.hpp"
#include "aeronet/errno-throw.hpp"
#include "aeronet/event.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/system-error-message.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

namespace {

static_assert(std::is_trivially_copyable_v<EventLoop::EventFd> && std::is_standard_layout_v<EventLoop::EventFd>,
              "EventLoop::EventFd must be trivially copyable for malloc / realloc usage");

#ifdef AERONET_LINUX
static_assert(std::is_trivially_copyable_v<epoll_event> && std::is_standard_layout_v<epoll_event>,
              "epoll_event must be trivially copyable for malloc / realloc usage");
static_assert(sizeof(epoll_event) >= sizeof(EventLoop::EventFd),
              "EventLoop requires epoll_event to be at least as large as EventFd for the convert loop");

static_assert(EventIn == EPOLLIN, "EventIn value mismatch");
static_assert(EventOut == EPOLLOUT, "EventOut value mismatch");
static_assert(EventErr == EPOLLERR, "EventErr value mismatch");
static_assert(EventHup == EPOLLHUP, "EventHup value mismatch");
static_assert(EventRdHup == EPOLLRDHUP, "EventRdHup value mismatch");
static_assert(EventEt == EPOLLET, "EventEt value mismatch");
#endif

#ifdef AERONET_MACOS
// The native event buffer element size for kqueue.
// struct kevent is typically 64 bytes on macOS (larger than EventFd) so in-place conversion is safe.
static_assert(sizeof(struct kevent) >= sizeof(EventLoop::EventFd),
              "EventLoop requires kevent to be at least as large as EventFd for the convert loop");

EventBmp KqueueFilterToEventBmp(const struct kevent& kev) {
  EventBmp bmp = 0;
  if (kev.filter == EVFILT_READ) {
    bmp |= EventIn;
  } else if (kev.filter == EVFILT_WRITE) {
    bmp |= EventOut;
  }
  if (kev.flags & EV_EOF) {
    bmp |= EventHup;
    // EV_EOF on a read filter is analogous to EPOLLRDHUP
    if (kev.filter == EVFILT_READ) {
      bmp |= EventRdHup;
    }
  }
  if (kev.flags & EV_ERROR) {
    bmp |= EventErr;
  }
  return bmp;
}
#endif  // AERONET_MACOS

#ifdef AERONET_LINUX
std::size_t NativeEventSize() { return sizeof(epoll_event); }
#elifdef AERONET_MACOS
std::size_t NativeEventSize() { return sizeof(struct kevent); }
#elifdef AERONET_WINDOWS
// WSAPoll: EventFd[] output buffer uses sizeof(EventFd) directly.
std::size_t NativeEventSize() { return sizeof(EventLoop::EventFd); }

short EventBmpToPollEvents(EventBmp bmp) {
  short ev = 0;
  if (bmp & EventIn) {
    ev |= POLLIN;
  }
  if (bmp & EventOut) {
    ev |= POLLOUT;
  }
  // EventEt, EventRdHup, EventErr, EventHup are not settable in poll; output-only or unsupported.
  return ev;
}

EventBmp PollReventsToEventBmp(short revents) {
  EventBmp bmp = 0;
  if (revents & POLLIN) {
    bmp |= EventIn;
  }
  if (revents & POLLOUT) {
    bmp |= EventOut;
  }
  if (revents & POLLERR) {
    bmp |= EventErr;
  }
  if (revents & POLLHUP) {
    bmp |= EventHup | EventRdHup;
  }
  if (revents & POLLNVAL) {
    bmp |= EventErr;
  }
  return bmp;
}
#endif

int DurationToPollTimeoutMs(SysDuration timeout) {
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
  assert(milliseconds >= 0);
  assert(!std::cmp_greater(milliseconds, std::numeric_limits<int>::max()));
  return static_cast<int>(milliseconds);
}

int MultiplyTimeoutWithCap(int timeoutMs, uint32_t growthFactor, int maxTimeoutMs) noexcept {
  const int floorMs = std::max(timeoutMs, 1);
  const auto scaledTimeoutMs = static_cast<std::uint64_t>(floorMs) * static_cast<std::uint64_t>(growthFactor);
  if (scaledTimeoutMs >= static_cast<std::uint64_t>(maxTimeoutMs)) {
    return maxTimeoutMs;
  }
  return static_cast<int>(scaledTimeoutMs);
}

}  // namespace

// ---- Construction / move / destruction ----

EventLoop::EventLoop(PollTimeoutPolicy timeoutPolicy, uint32_t initialCapacity)
    : _nbAllocatedEvents(std::max(1U, initialCapacity)),
      _pEvents(std::malloc(static_cast<std::size_t>(_nbAllocatedEvents) * NativeEventSize())) {
  timeoutPolicy.validate();
  if (_pEvents == nullptr) {
    throw std::bad_alloc();
  }
  updatePollTimeoutPolicy(timeoutPolicy);

#ifdef AERONET_LINUX
  _baseFd = BaseFd(::epoll_create1(EPOLL_CLOEXEC));
  if (!_baseFd) {
    auto err = LastSystemError();
    log::error("event loop creation failed (err={}, msg={})", err, SystemErrorMessage(err));
    std::free(_pEvents);
    _pEvents = nullptr;
    throw std::runtime_error("event loop creation failed");
  }
#elifdef AERONET_MACOS
  _baseFd = BaseFd(::kqueue());
  if (!_baseFd) {
    auto err = LastSystemError();
    log::error("kqueue creation failed (err={}, msg={})", err, SystemErrorMessage(err));
    std::free(_pEvents);
    _pEvents = nullptr;
    throw std::runtime_error("event loop creation failed");
  }
#elifdef AERONET_WINDOWS
  // WSAPoll doesn't need a kernel object like epoll/kqueue.
  // Allocate the WSAPOLLFD registration array.
  _pPollFds = std::malloc(static_cast<std::size_t>(_nbAllocatedEvents) * sizeof(WSAPOLLFD));
  if (_pPollFds == nullptr) {
    std::free(_pEvents);
    _pEvents = nullptr;
    throw std::bad_alloc();
  }
#endif

  if (initialCapacity == 0) {
    log::warn("EventLoop constructed with initialCapacity=0; promoting to 1");
  }

#ifndef AERONET_WINDOWS
  log::debug("EventLoop fd # {} opened", static_cast<intptr_t>(_baseFd.fd()));
#else
  log::debug("EventLoop WSAPoll initialized (capacity={})", _nbAllocatedEvents);
#endif
}

EventLoop::EventLoop(EventLoop&& rhs) noexcept
    : _nbAllocatedEvents(std::exchange(rhs._nbAllocatedEvents, 0)),
      _pollTimeoutMs(rhs._pollTimeoutMs),
      _basePollTimeoutMs(rhs._basePollTimeoutMs),
      _minPollTimeoutMs(rhs._minPollTimeoutMs),
      _maxPollTimeoutMs(rhs._maxPollTimeoutMs),
      _idlePollIterations(rhs._idlePollIterations),
      _baseFd(std::move(rhs._baseFd)),
      _pEvents(std::exchange(rhs._pEvents, nullptr))
#ifdef AERONET_WINDOWS
      ,
      _pPollFds(std::exchange(rhs._pPollFds, nullptr)),
      _nbRegistered(std::exchange(rhs._nbRegistered, 0U))
#endif
{
}

EventLoop& EventLoop::operator=(EventLoop&& rhs) noexcept {
  if (this != &rhs) [[likely]] {
    std::free(_pEvents);
#ifdef AERONET_WINDOWS
    std::free(_pPollFds);
#endif

    _nbAllocatedEvents = std::exchange(rhs._nbAllocatedEvents, 0);
    _pollTimeoutMs = rhs._pollTimeoutMs;
    _basePollTimeoutMs = rhs._basePollTimeoutMs;
    _minPollTimeoutMs = rhs._minPollTimeoutMs;
    _maxPollTimeoutMs = rhs._maxPollTimeoutMs;
    _idlePollIterations = rhs._idlePollIterations;
    _baseFd = std::move(rhs._baseFd);
    _pEvents = std::exchange(rhs._pEvents, nullptr);
#ifdef AERONET_WINDOWS
    _pPollFds = std::exchange(rhs._pPollFds, nullptr);
    _nbRegistered = std::exchange(rhs._nbRegistered, 0U);
#endif
  }
  return *this;
}

EventLoop::~EventLoop() {
  std::free(_pEvents);
#ifdef AERONET_WINDOWS
  std::free(_pPollFds);
#endif
}

// ---- add / mod / del ----

void EventLoop::addOrThrow(EventFd event) {
  if (!add(event)) [[unlikely]] {
    ThrowSystemError("epoll_ctl ADD failed (fd # {}, events=0x{:x})", event.fd, event.eventBmp);
  }
}

bool EventLoop::add(EventFd event) {
#ifdef AERONET_LINUX
  epoll_event ev{event.eventBmp, epoll_data_t{.fd = event.fd}};
  if (::epoll_ctl(_baseFd.fd(), EPOLL_CTL_ADD, event.fd, &ev) != 0) [[unlikely]] {
    const auto err = LastSystemError();
    log::error("epoll_ctl ADD failed (fd # {}, events=0x{:x}, err={}, msg={})", event.fd, event.eventBmp, err,
               SystemErrorMessage(err));
    return false;
  }
#elifdef AERONET_MACOS
  // On kqueue, register separate EVFILT_READ and/or EVFILT_WRITE filters.
  // EV_CLEAR is the kqueue equivalent of edge-triggered mode.
  struct kevent changes[2];
  int nchanges = 0;
  unsigned short flags = EV_ADD | EV_ENABLE;
  if (event.eventBmp & EventEt) {
    flags |= EV_CLEAR;
  }
  if (event.eventBmp & EventIn) {
    EV_SET(&changes[nchanges++], static_cast<uintptr_t>(event.fd), EVFILT_READ, flags, 0, 0,
           reinterpret_cast<void*>(static_cast<intptr_t>(event.fd)));
  }
  if (event.eventBmp & EventOut) {
    EV_SET(&changes[nchanges++], static_cast<uintptr_t>(event.fd), EVFILT_WRITE, flags, 0, 0,
           reinterpret_cast<void*>(static_cast<intptr_t>(event.fd)));
  }
  if (nchanges == 0) {
    // At least register for read by default
    EV_SET(&changes[nchanges++], static_cast<uintptr_t>(event.fd), EVFILT_READ, flags, 0, 0,
           reinterpret_cast<void*>(static_cast<intptr_t>(event.fd)));
  }
  if (::kevent(_baseFd.fd(), changes, nchanges, nullptr, 0, nullptr) == -1) [[unlikely]] {
    const auto err = LastSystemError();
    log::error("kevent ADD failed (fd # {}, events=0x{:x}, err={}, msg={})", event.fd, event.eventBmp, err,
               SystemErrorMessage(err));
    return false;
  }
#elifdef AERONET_WINDOWS
  // Grow both buffers if the registration array is full.
  if (_nbRegistered == _nbAllocatedEvents) {
    const uint32_t newCap = _nbAllocatedEvents * 2U;
    void* newPollFds = std::realloc(_pPollFds, static_cast<std::size_t>(newCap) * sizeof(WSAPOLLFD));
    if (newPollFds == nullptr) {
      log::error("Failed to grow WSAPoll fd array from {} to {}", _nbAllocatedEvents, newCap);
      return false;
    }
    _pPollFds = newPollFds;
    void* newEvents = std::realloc(_pEvents, static_cast<std::size_t>(newCap) * NativeEventSize());
    if (newEvents == nullptr) {
      log::error("Failed to grow WSAPoll event buffer from {} to {}", _nbAllocatedEvents, newCap);
      return false;
    }
    _pEvents = newEvents;
    _nbAllocatedEvents = newCap;
  }
  auto* pollFds = static_cast<WSAPOLLFD*>(_pPollFds);
  pollFds[_nbRegistered].fd = event.fd;
  pollFds[_nbRegistered].events = EventBmpToPollEvents(event.eventBmp);
  pollFds[_nbRegistered].revents = 0;
  ++_nbRegistered;
#endif
  return true;
}

bool EventLoop::mod(EventFd event) {
#ifdef AERONET_LINUX
  epoll_event ev{event.eventBmp, epoll_data_t{.fd = event.fd}};
  if (::epoll_ctl(_baseFd.fd(), EPOLL_CTL_MOD, event.fd, &ev) != 0) [[unlikely]] {
    const auto err = LastSystemError();
    // EBADF or ENOENT can occur during races where a connection is concurrently closed; downgrade severity.
    if (err == EBADF || err == ENOENT) {
      log::warn("epoll_ctl MOD benign failure (fd # {}, events=0x{:x}, err={}, msg={})", event.fd, event.eventBmp, err,
                SystemErrorMessage(err));
    } else {
      log::error("epoll_ctl MOD failed (fd # {}, events=0x{:x}, err={}, msg={})", event.fd, event.eventBmp, err,
                 SystemErrorMessage(err));
    }
    return false;
  }
  return true;
#elifdef AERONET_MACOS
  // kqueue: EV_ADD on an existing filter replaces it (acts like MOD).
  return add(event);
#elifdef AERONET_WINDOWS
  auto* pollFds = static_cast<WSAPOLLFD*>(_pPollFds);
  for (uint32_t idx = 0; idx < _nbRegistered; ++idx) {
    if (pollFds[idx].fd == event.fd) {
      pollFds[idx].events = EventBmpToPollEvents(event.eventBmp);
      return true;
    }
  }
  log::error("EventLoop::mod fd # {} not found in WSAPoll set", static_cast<uintptr_t>(event.fd));
  return false;
#endif
}

void EventLoop::del(NativeHandle fd) {
#ifdef AERONET_LINUX
  if (::epoll_ctl(_baseFd.fd(), EPOLL_CTL_DEL, fd, nullptr) != 0) [[unlikely]] {
    // DEL failures are usually benign if fd already closed; log at debug to avoid noise.
    const auto err = LastSystemError();
    log::debug("epoll_ctl DEL failed (fd # {}, err={}, msg={})", fd, err, SystemErrorMessage(err));
  }

#elifdef AERONET_MACOS
  // Unregister both filters. Errors are benign if the fd was already closed (kqueue auto-removes).
  struct kevent changes[2];
  EV_SET(&changes[0], static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  EV_SET(&changes[1], static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
  if (::kevent(_baseFd.fd(), changes, 2, nullptr, 0, nullptr) == -1) [[unlikely]] {
    const auto err = LastSystemError();
    log::debug("kevent DEL failed (fd # {}, err={}, msg={})", fd, err, SystemErrorMessage(err));
  }

#elifdef AERONET_WINDOWS
  auto* pollFds = static_cast<WSAPOLLFD*>(_pPollFds);
  for (uint32_t idx = 0; idx < _nbRegistered; ++idx) {
    if (pollFds[idx].fd == fd) {
      pollFds[idx] = pollFds[--_nbRegistered];
      return;
    }
  }
  log::debug("EventLoop::del fd # {} not found in WSAPoll set", static_cast<uintptr_t>(fd));
#endif
}

// ---- poll ----

std::span<const EventLoop::EventFd> EventLoop::poll() {
  const uint32_t capacityBeforePoll = _nbAllocatedEvents;

#ifdef AERONET_LINUX
  auto* epollEvents = static_cast<epoll_event*>(_pEvents);
  const int nbReadyFds = ::epoll_wait(_baseFd.fd(), epollEvents, static_cast<int>(capacityBeforePoll), _pollTimeoutMs);

  if (nbReadyFds == -1) {
    if (LastSystemError() == error::kInterrupted) {
      return {std::launder(reinterpret_cast<EventFd*>(_pEvents)), 0U};
    }
    const auto err = LastSystemError();
    log::error("epoll_wait failed (timeout_ms={}, err={}, msg={})", _pollTimeoutMs, err, SystemErrorMessage(err));
    return {};
  }

  const uint32_t nbReadyEvents = static_cast<uint32_t>(nbReadyFds);

#elifdef AERONET_MACOS
  auto* kevents = static_cast<struct kevent*>(_pEvents);
  struct timespec ts;
  ts.tv_sec = _pollTimeoutMs / 1000;
  ts.tv_nsec = (_pollTimeoutMs % 1000) * 1000000L;

  const int nbReadyFds = ::kevent(_baseFd.fd(), nullptr, 0, kevents, static_cast<int>(capacityBeforePoll), &ts);

  if (nbReadyFds == -1) {
    if (LastSystemError() == error::kInterrupted) {
      return {std::launder(reinterpret_cast<EventFd*>(_pEvents)), 0U};
    }
    const auto err = LastSystemError();
    log::error("kevent failed (timeout_ms={}, err={}, msg={})", _pollTimeoutMs, err, SystemErrorMessage(err));
    return {};
  }

  const uint32_t nbReadyEvents = static_cast<uint32_t>(nbReadyFds);

#elifdef AERONET_WINDOWS
  auto* pollFdsBuf = static_cast<WSAPOLLFD*>(_pPollFds);
  const int nbReadyFds = ::WSAPoll(pollFdsBuf, _nbRegistered, _pollTimeoutMs);

  if (nbReadyFds == SOCKET_ERROR) {
    auto err = LastSystemError();
    if (err == error::kInterrupted) {
      return {std::launder(reinterpret_cast<EventFd*>(_pEvents)), 0U};
    }
    log::error("WSAPoll failed (err={}, msg={})", err, SystemErrorMessage(err));
    return {};
  }

  const uint32_t nbReadyEvents = static_cast<uint32_t>(nbReadyFds);

#endif

  updateAdaptivePollTimeout(nbReadyEvents, capacityBeforePoll);

  // If saturated, grow buffer for subsequent polls.
  // On Windows, buffer growth is handled in add() when registration count reaches capacity.
#ifndef AERONET_WINDOWS
  if (nbReadyEvents == capacityBeforePoll) {
    const uint32_t newCapacity = capacityBeforePoll * 2U;
    void* newEvents = std::realloc(_pEvents, static_cast<std::size_t>(newCapacity) * NativeEventSize());
    if (newEvents == nullptr) {
      log::error("Failed to reallocate memory for saturated events, keeping actual size of {}", _nbAllocatedEvents);
    } else {
      _pEvents = newEvents;
      _nbAllocatedEvents = newCapacity;
    }
  }
#endif

  EventFd* out = std::launder(reinterpret_cast<EventFd*>(_pEvents));

#ifdef AERONET_LINUX
  // Convert epoll_event[] into EventFd[] in-place (EventFd is smaller or equal in size/alignment).
  if constexpr (offsetof(epoll_event, data.fd) != offsetof(EventFd, fd) ||
                offsetof(epoll_event, events) != offsetof(EventFd, eventBmp) ||
                sizeof(epoll_event) != sizeof(EventFd)) {
    auto* epollEventsOut = static_cast<epoll_event*>(_pEvents);
    for (uint32_t idx = 0; idx < nbReadyEvents; ++idx) {
      out[idx] = EventFd{epollEventsOut[idx].data.fd, static_cast<EventBmp>(epollEventsOut[idx].events)};
    }
  }
#elifdef AERONET_MACOS
  // Convert kevent[] into EventFd[] in-place.
  auto* keventsOut = static_cast<struct kevent*>(_pEvents);
  for (uint32_t idx = 0; idx < nbReadyEvents; ++idx) {
    auto nativeHandle = static_cast<NativeHandle>(keventsOut[idx].ident);
    EventBmp bmp = KqueueFilterToEventBmp(keventsOut[idx]);
    out[idx] = EventFd{nativeHandle, bmp};
  }
#elifdef AERONET_WINDOWS
  // Convert ready WSAPOLLFD entries into EventFd[] output buffer.
  auto* pollFdsConv = static_cast<WSAPOLLFD*>(_pPollFds);
  uint32_t outIdx = 0;
  for (uint32_t idx = 0; idx < _nbRegistered && outIdx < nbReadyEvents; ++idx) {
    if (pollFdsConv[idx].revents != 0) {
      out[outIdx++] = EventFd{pollFdsConv[idx].fd, PollReventsToEventBmp(pollFdsConv[idx].revents)};
    }
  }
#endif

  return {out, nbReadyEvents};
}

void EventLoop::PollTimeoutPolicy::validate() const {
  const auto minMs = std::chrono::duration_cast<std::chrono::milliseconds>(minTimeout).count();
  const auto baseMs = std::chrono::duration_cast<std::chrono::milliseconds>(baseTimeout).count();
  const auto maxMs = std::chrono::duration_cast<std::chrono::milliseconds>(maxTimeout).count();
  if (baseMs <= 0) {
    throw std::invalid_argument("PollTimeoutPolicy: baseTimeout must be > 0");
  }
  if (minMs < 0) {
    throw std::invalid_argument("PollTimeoutPolicy: minTimeout must be non-negative");
  }
  if (minMs > baseMs) {
    throw std::invalid_argument("PollTimeoutPolicy: minTimeout must be <= baseTimeout");
  }
  if (maxMs < baseMs) {
    throw std::invalid_argument("PollTimeoutPolicy: maxTimeout must be >= baseTimeout");
  }
  if (std::cmp_greater(maxMs, std::numeric_limits<int>::max())) {
    throw std::invalid_argument("PollTimeoutPolicy: maxTimeout exceeds poll API limit (INT_MAX ms)");
  }
}

void EventLoop::updatePollTimeoutPolicy(PollTimeoutPolicy timeoutPolicy) {
  _idlePollIterations = 0;

  _minPollTimeoutMs = DurationToPollTimeoutMs(timeoutPolicy.minTimeout);
  _maxPollTimeoutMs = DurationToPollTimeoutMs(timeoutPolicy.maxTimeout);
  assert(_maxPollTimeoutMs >= _minPollTimeoutMs);
  _basePollTimeoutMs = DurationToPollTimeoutMs(timeoutPolicy.baseTimeout);
  assert(_basePollTimeoutMs >= _minPollTimeoutMs && _basePollTimeoutMs <= _maxPollTimeoutMs);

  _pollTimeoutMs = _basePollTimeoutMs;
}

void EventLoop::updateAdaptivePollTimeout(uint32_t nbReadyEvents, uint32_t capacityBeforePoll) noexcept {
  // Saturation: every event slot was filled. Drop to the minimum (often 0) so the next poll
  // returns immediately and we drain whatever else is waiting in the kernel queue.
  assert(capacityBeforePoll > 0U);
  if (nbReadyEvents == capacityBeforePoll) {
    _pollTimeoutMs = _minPollTimeoutMs;
    _idlePollIterations = 0;
    return;
  }

  // Idle: no events at all. Increase the timeout exponentially after enough consecutive idle
  // polls, capped at maxTimeout. Note: when transitioning saturation -> idle the backoff grows
  // from the current (small) timeout rather than restarting at base; this is intentional and
  // gives a gentle decay back to the maximum sleep when the server truly goes quiet.
  if (nbReadyEvents == 0U) {
    static constexpr uint32_t kDefaultIdleIterationsBeforeBackoff = 4;
    static constexpr uint32_t kDefaultPollTimeoutGrowthFactor = 2;

    ++_idlePollIterations;
    if (_idlePollIterations >= kDefaultIdleIterationsBeforeBackoff) {
      _pollTimeoutMs = MultiplyTimeoutWithCap(_pollTimeoutMs, kDefaultPollTimeoutGrowthFactor, _maxPollTimeoutMs);
      _idlePollIterations = 0;
    }
    return;
  }

  // Normal load: some but not all slots used. Reset to the configured base so we neither spin
  // nor stay in backoff mode.
  _pollTimeoutMs = _basePollTimeoutMs;
  _idlePollIterations = 0;
}

}  // namespace aeronet
