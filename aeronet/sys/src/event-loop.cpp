#include "aeronet/event-loop.hpp"

#include "aeronet/platform.hpp"

#ifdef AERONET_LINUX
#include <sys/epoll.h>
#elifdef AERONET_MACOS
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#elifdef AERONET_WINDOWS
// IOCP headers included via platform.hpp (winsock2.h)
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
// IOCP uses OVERLAPPED_ENTRY for GetQueuedCompletionStatusEx
std::size_t NativeEventSize() { return sizeof(OVERLAPPED_ENTRY); }
#endif

}  // namespace

// ---- Construction / move / destruction ----

EventLoop::EventLoop(SysDuration pollTimeout, uint32_t initialCapacity)
    : _nbAllocatedEvents(std::max(1U, initialCapacity)),
      _pollTimeoutMs(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(pollTimeout).count())) {
  _pEvents = std::malloc(static_cast<std::size_t>(_nbAllocatedEvents) * NativeEventSize());
  if (_pEvents == nullptr) {
    throw std::bad_alloc();
  }

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
  // Create an I/O Completion Port with no initial file handle association.
  HANDLE iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
  if (iocp == nullptr) {
    auto err = ::GetLastError();
    log::error("CreateIoCompletionPort failed (error={})", err);
    std::free(_pEvents);
    _pEvents = nullptr;
    throw std::runtime_error("event loop creation failed");
  }
  _baseFd = BaseFd(reinterpret_cast<NativeHandle>(iocp), BaseFd::HandleKind::Win32Handle);
#endif

  if (initialCapacity == 0) {
    log::warn("EventLoop constructed with initialCapacity=0; promoting to 1");
  }

  log::debug("EventLoop fd # {} opened", static_cast<intptr_t>(_baseFd.fd()));
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

EventLoop::~EventLoop() {
  // BaseFd::close() handles Windows IOCP HANDLE via HandleKind::Win32Handle.
  std::free(_pEvents);
}

// ---- add / mod / del ----

void EventLoop::addOrThrow(EventFd event) const {
  if (!add(event)) [[unlikely]] {
    throw_errno("event loop ADD failed (fd # {}, events=0x{:x})", static_cast<intptr_t>(event.fd), event.eventBmp);
  }
}

bool EventLoop::add(EventFd event) const {
#ifdef AERONET_LINUX
  epoll_event ev{event.eventBmp, epoll_data_t{.fd = event.fd}};
  if (::epoll_ctl(_baseFd.fd(), EPOLL_CTL_ADD, event.fd, &ev) != 0) [[unlikely]] {
    const auto err = LastSystemError();
    log::error("epoll_ctl ADD failed (fd # {}, events=0x{:x}, err={}, msg={})", event.fd, event.eventBmp, err,
               SystemErrorMessage(err));
    return false;
  }
  return true;

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
  return true;

#elifdef AERONET_WINDOWS
  // Associate the handle with the IOCP. The completion key stores the fd value for dispatch.
  HANDLE iocp = reinterpret_cast<HANDLE>(_baseFd.fd());
  HANDLE result =
      ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(event.fd), iocp, static_cast<ULONG_PTR>(event.fd), 0);
  if (result == nullptr) [[unlikely]] {
    auto err = ::GetLastError();
    log::error("CreateIoCompletionPort ADD failed (fd # {}, events=0x{:x}, error={})", static_cast<uintptr_t>(event.fd),
               event.eventBmp, err);
    return false;
  }
  return true;
#endif
}

bool EventLoop::mod(EventFd event) const {
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
  // IOCP doesn't support modifying events — once associated, persistence is managed
  // by submitting new overlapped operations. This is a stub for the readiness-model API.
  log::debug("EventLoop::mod is a no-op on Windows (IOCP model) for fd # {}", static_cast<uintptr_t>(event.fd));
  return true;
#endif
}

void EventLoop::del(NativeHandle fd) const {
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
  // IOCP auto-cleans when the handle is closed. Explicit removal is not supported.
  log::debug("EventLoop::del is a no-op on Windows (IOCP model) for fd # {}", static_cast<uintptr_t>(fd));
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

  // If saturated, grow buffer for subsequent polls.
  if (std::cmp_equal(nbReadyFds, capacityBeforePoll)) {
    const uint32_t newCapacity = capacityBeforePoll * 2U;
    void* newEvents = std::realloc(_pEvents, static_cast<std::size_t>(newCapacity) * sizeof(struct kevent));
    if (newEvents == nullptr) {
      log::error("Failed to reallocate memory for saturated events, keeping actual size of {}", _nbAllocatedEvents);
    } else {
      _pEvents = newEvents;
      _nbAllocatedEvents = newCapacity;
    }
  }

  // Convert kevent[] into EventFd[] in-place.
  EventFd* out = std::launder(reinterpret_cast<EventFd*>(_pEvents));
  kevents = static_cast<struct kevent*>(_pEvents);
  for (int idx = 0; idx < nbReadyFds; ++idx) {
    auto nativeHandle = static_cast<NativeHandle>(kevents[idx].ident);
    EventBmp bmp = KqueueFilterToEventBmp(kevents[idx]);
    out[idx] = EventFd{nativeHandle, bmp};
  }

  return {out, static_cast<std::size_t>(nbReadyFds)};

#elifdef AERONET_WINDOWS
  auto* entries = static_cast<OVERLAPPED_ENTRY*>(_pEvents);
  ULONG nbRemoved = 0;
  BOOL ok = ::GetQueuedCompletionStatusEx(reinterpret_cast<HANDLE>(_baseFd.fd()), entries, capacityBeforePoll,
                                          &nbRemoved, static_cast<DWORD>(_pollTimeoutMs), FALSE);
  if (!ok) {
    DWORD err = ::GetLastError();
    if (err == WAIT_TIMEOUT) {
      return {std::launder(reinterpret_cast<EventFd*>(_pEvents)), 0U};
    }
    log::error("GetQueuedCompletionStatusEx failed (error={})", err);
    return {};
  }

  // If saturated, grow buffer.
  if (nbRemoved == capacityBeforePoll) {
    const uint32_t newCapacity = capacityBeforePoll * 2U;
    void* newEvents = std::realloc(_pEvents, static_cast<std::size_t>(newCapacity) * sizeof(OVERLAPPED_ENTRY));
    if (newEvents == nullptr) {
      log::error("Failed to reallocate memory for saturated events, keeping actual size of {}", _nbAllocatedEvents);
    } else {
      _pEvents = newEvents;
      _nbAllocatedEvents = newCapacity;
    }
  }

  // Convert OVERLAPPED_ENTRY[] into EventFd[] in-place.
  // For now, map completions to EventIn (read readiness) as a placeholder.
  EventFd* out = std::launder(reinterpret_cast<EventFd*>(_pEvents));
  for (ULONG idx = 0; idx < nbRemoved; ++idx) {
    auto nativeHandle = static_cast<NativeHandle>(entries[idx].lpCompletionKey);
    out[idx] = EventFd{nativeHandle, EventIn};
  }

  return {out, static_cast<std::size_t>(nbRemoved)};
#endif
}

void EventLoop::updatePollTimeout(SysDuration pollTimeout) {
  _pollTimeoutMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(pollTimeout).count());
}

}  // namespace aeronet
