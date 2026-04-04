#include "aeronet/event-loop.hpp"

#ifdef AERONET_IO_URING
#include <liburing.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#elifdef AERONET_LINUX
#include <sys/epoll.h>
#elifdef AERONET_MACOS
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#elifdef AERONET_WINDOWS
// WSAPoll headers included via winsock2.h
#endif

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
#include "aeronet/native-handle.hpp"
#include "aeronet/system-error-message.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

namespace {

static_assert(std::is_trivially_copyable_v<EventLoop::EventFd> && std::is_standard_layout_v<EventLoop::EventFd>,
              "EventLoop::EventFd must be trivially copyable for malloc / realloc usage");

#ifdef AERONET_IO_URING
// io_uring poll uses standard poll(2) event masks. On Linux these match the EPOLL* values
// for the common flags (POLLIN==EPOLLIN, POLLOUT==EPOLLOUT, etc.). Verify at compile time.
static_assert(EventIn == POLLIN, "EventIn must match POLLIN for io_uring poll");
static_assert(EventOut == POLLOUT, "EventOut must match POLLOUT for io_uring poll");
static_assert(EventErr == POLLERR, "EventErr must match POLLERR for io_uring poll");
static_assert(EventHup == POLLHUP, "EventHup must match POLLHUP for io_uring poll");
static_assert(EventRdHup == POLLRDHUP, "EventRdHup must match POLLRDHUP for io_uring poll");

struct IoUring {
  struct io_uring ring;
  // Buffer for storing packed user_data (fd + gen + mask) of events that need re-arming.
  // Allocated alongside _pEvents and grown in sync.
  uint64_t* rearmBuf = nullptr;
  // Number of entries in rearmBuf that need re-arming at the start of the next poll() call.
  // Deferred re-arm avoids re-triggering polls before the caller can consume event data.
  uint32_t nbRearmPending = 0;
  // Per-fd generation counter.  Incremented on every add/mod/del so stale CQEs from a
  // previous incarnation of the same fd number are discarded during harvest.
  // This solves the ABA problem where the kernel recycles an fd and a stale CQE passes
  // through the boolean active check.
  uint16_t* fdGen = nullptr;
  uint32_t fdGenCap = 0;
  // Listen fd for multishot accept (-1 if not active).
  int acceptListenFd = -1;
  // Tracks whether there are un-submitted SQEs in the SQ ring.
  // When true, poll() will call io_uring_submit() before waiting.
  bool hasPendingSqes = false;
};

// Ensure fdGen is large enough to hold index 'fd'. Returns false on OOM or invalid fd.
bool EnsureFdGenCap(IoUring* iouring, int fd) {
  if (fd < 0) [[unlikely]] {
    return false;
  }
  const auto idx = static_cast<uint32_t>(fd);
  if (idx < iouring->fdGenCap) {
    return true;
  }
  const uint32_t newCap = std::max(idx + 1, iouring->fdGenCap * 2);
  auto* buf = static_cast<uint16_t*>(std::realloc(iouring->fdGen, newCap * sizeof(uint16_t)));
  if (buf == nullptr) [[unlikely]] {
    return false;
  }
  std::memset(buf + iouring->fdGenCap, 0, (newCap - iouring->fdGenCap) * sizeof(uint16_t));
  iouring->fdGen = buf;
  iouring->fdGenCap = newCap;
  return true;
}

// Pack fd + generation + mask into a single uint64_t for single-shot poll SQE user_data.
// Bits  0-31: fd
// Bits 32-47: generation counter (disambiguates stale CQEs after fd reuse)
// Bits 48-63: original poll mask
// Cancel SQEs use kCancelSentinel as their own user_data.
// Multishot accept SQEs use kAcceptMask in bits 48-63 to distinguish from poll CQEs.
constexpr uint64_t kCancelSentinel = UINT64_MAX;
constexpr uint32_t kAcceptMask = 0xACCE;  // Mnemonic: "ACCEpt"
constexpr uint32_t kCloseMask = 0xC105;   // Mnemonic: "CLOSe"

uint64_t PackUserData(int fd, uint32_t mask, uint16_t gen) {
  return static_cast<uint64_t>(static_cast<uint32_t>(fd)) | (static_cast<uint64_t>(gen) << 32) |
         (static_cast<uint64_t>(mask) << 48);
}

int UnpackFd(uint64_t ud) { return static_cast<int>(static_cast<uint32_t>(ud & 0xFFFFFFFF)); }

uint16_t UnpackGen(uint64_t ud) { return static_cast<uint16_t>(ud >> 32); }

uint32_t UnpackMask(uint64_t ud) { return static_cast<uint32_t>(ud >> 48); }

// Try to get an SQE from the ring. If the SQ is full, flush pending SQEs and retry.
// Returns nullptr only on unrecoverable failure.
struct io_uring_sqe* GetSqe(IoUring* iouring) {
  struct io_uring_sqe* sqe = ::io_uring_get_sqe(&iouring->ring);
  if (sqe == nullptr) [[unlikely]] {
    // SQ full — flush pending SQEs to make room.
    ::io_uring_submit(&iouring->ring);
    iouring->hasPendingSqes = false;
    sqe = ::io_uring_get_sqe(&iouring->ring);
  }
  if (sqe != nullptr) {
    iouring->hasPendingSqes = true;
  }
  return sqe;
}
#elifdef AERONET_LINUX
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

#ifdef AERONET_IO_URING
// Output buffer uses EventFd directly (CQEs are converted into EventFd on harvest).
std::size_t NativeEventSize() { return sizeof(EventLoop::EventFd); }
#elifdef AERONET_LINUX
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

}  // namespace

// ---- Construction / move / destruction ----

EventLoop::EventLoop(SysDuration pollTimeout, uint32_t initialCapacity)
    : _nbAllocatedEvents(std::max(1U, initialCapacity)),
      _pollTimeoutMs(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(pollTimeout).count())),
      _pEvents(std::malloc(static_cast<std::size_t>(_nbAllocatedEvents) * NativeEventSize())) {
  if (_pEvents == nullptr) {
    throw std::bad_alloc();
  }

#ifdef AERONET_IO_URING
  auto* iouring = new (std::nothrow) IoUring();
  if (iouring == nullptr) {
    std::free(_pEvents);
    _pEvents = nullptr;
    throw std::bad_alloc();
  }
  iouring->rearmBuf =
      static_cast<uint64_t*>(std::malloc(static_cast<std::size_t>(_nbAllocatedEvents) * sizeof(uint64_t)));
  if (iouring->rearmBuf == nullptr) {
    delete iouring;
    std::free(_pEvents);
    _pEvents = nullptr;
    throw std::bad_alloc();
  }
  const int ret = [&]() {
    // Use a generous CQ size to prevent CQ overflow.  Overflow causes stale CQEs
    // to be delivered later for fd numbers that may have been reused, leading to
    // phantom events and re-arms on wrong descriptors.
    struct io_uring_params params{};
    params.flags = IORING_SETUP_CQSIZE;
    params.cq_entries = std::max(_nbAllocatedEvents * 8U, 4096U);
    return ::io_uring_queue_init_params(std::max(_nbAllocatedEvents, 64U), &iouring->ring, &params);
  }();
  if (ret < 0) {
    log::error("io_uring_queue_init_params failed (entries={}, err={}, msg={})", _nbAllocatedEvents, -ret,
               SystemErrorMessage(-ret));
    std::free(iouring->rearmBuf);
    delete iouring;
    std::free(_pEvents);
    _pEvents = nullptr;
    throw std::runtime_error("io_uring initialization failed");
  }
  _pRing = iouring;
  // Store the ring fd in _baseFd for logging / diagnostics (not used for I/O).
  _baseFd = BaseFd(iouring->ring.ring_fd);
#elifdef AERONET_LINUX
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
      _baseFd(std::move(rhs._baseFd)),
      _pEvents(std::exchange(rhs._pEvents, nullptr))
#ifdef AERONET_IO_URING
      ,
      _pRing(std::exchange(rhs._pRing, nullptr))
#endif
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
#ifdef AERONET_IO_URING
    if (_pRing != nullptr) {
      auto* iouring = static_cast<IoUring*>(_pRing);
      (void)_baseFd.release();  // Prevent double-close: io_uring_queue_exit closes the ring fd.
      ::io_uring_queue_exit(&iouring->ring);
      std::free(iouring->rearmBuf);
      std::free(iouring->fdGen);
      delete iouring;
    }
#endif
#ifdef AERONET_WINDOWS
    std::free(_pPollFds);
#endif

    _nbAllocatedEvents = std::exchange(rhs._nbAllocatedEvents, 0);
    _pollTimeoutMs = rhs._pollTimeoutMs;
    _baseFd = std::move(rhs._baseFd);
    _pEvents = std::exchange(rhs._pEvents, nullptr);
#ifdef AERONET_IO_URING
    _pRing = std::exchange(rhs._pRing, nullptr);
#endif
#ifdef AERONET_WINDOWS
    _pPollFds = std::exchange(rhs._pPollFds, nullptr);
    _nbRegistered = std::exchange(rhs._nbRegistered, 0U);
#endif
  }
  return *this;
}

EventLoop::~EventLoop() {
#ifdef AERONET_IO_URING
  if (_pRing != nullptr) {
    auto* iouring = static_cast<IoUring*>(_pRing);
    // Prevent BaseFd from closing the ring fd (io_uring_queue_exit does it).
    (void)_baseFd.release();
    ::io_uring_queue_exit(&iouring->ring);
    std::free(iouring->rearmBuf);
    std::free(iouring->fdGen);
    delete iouring;
  }
#endif
  std::free(_pEvents);
#ifdef AERONET_WINDOWS
  std::free(_pPollFds);
#endif
}

// ---- add / mod / del ----

void EventLoop::addOrThrow(EventFd event) {
  if (!add(event)) [[unlikely]] {
    ThrowSystemError("EventLoop add failed (fd # {}, events=0x{:x})", event.fd, event.eventBmp);
  }
}

bool EventLoop::add(EventFd event) {
#ifdef AERONET_IO_URING
  auto* iouring = static_cast<IoUring*>(_pRing);

  if (!EnsureFdGenCap(iouring, event.fd)) [[unlikely]] {
    log::error("Failed to grow fdGen for ADD (fd # {})", event.fd);
    return false;
  }

  struct io_uring_sqe* sqe = GetSqe(iouring);
  if (sqe == nullptr) [[unlikely]] {
    log::error("io_uring_get_sqe failed for ADD (fd # {}, events=0x{:x}) — SQ full after submit", event.fd,
               event.eventBmp);
    return false;
  }
  // Use single-shot poll. We manually re-arm after each CQE in poll() to get
  // edge-triggered semantics (multishot is level-triggered and causes CQE avalanches).
  const auto pollMask = static_cast<uint32_t>(event.eventBmp & EventPollMask);
  // Bump generation so any stale CQEs from a previous incarnation of this fd are discarded.
  const auto gen = ++iouring->fdGen[event.fd];
  ::io_uring_prep_poll_add(sqe, event.fd, pollMask);
  ::io_uring_sqe_set_data64(sqe, PackUserData(event.fd, pollMask, gen));
  // SQE is batched and will be submitted in the next poll() cycle.
#elifdef AERONET_LINUX
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
#ifdef AERONET_IO_URING
  // For io_uring single-shot poll, modifying the event mask requires cancelling
  // the existing poll (by user_data matching) and re-adding with the new mask.
  // Since we encode the mask in user_data, we need to cancel by the OLD user_data.
  // However, we don't track the old mask. Use IORING_ASYNC_CANCEL_FD to cancel by fd instead.
  auto* iouring = static_cast<IoUring*>(_pRing);

  if (!EnsureFdGenCap(iouring, event.fd)) [[unlikely]] {
    log::error("Failed to grow fdGen for MOD (fd # {})", event.fd);
    return false;
  }

  // Remove this fd from the deferred re-arm buffer (mask is being replaced).
  for (uint32_t idx = 0; idx < iouring->nbRearmPending; ++idx) {
    if (UnpackFd(iouring->rearmBuf[idx]) == event.fd) {
      iouring->rearmBuf[idx] = iouring->rearmBuf[--iouring->nbRearmPending];
      break;
    }
  }

  // Cancel any existing poll for this fd.
  struct io_uring_sqe* cancelSqe = GetSqe(iouring);
  if (cancelSqe == nullptr) [[unlikely]] {
    log::error("io_uring_get_sqe failed for MOD cancel (fd # {})", event.fd);
    return false;
  }
  ::io_uring_prep_cancel_fd(cancelSqe, event.fd, 0);
  ::io_uring_sqe_set_data64(cancelSqe, kCancelSentinel);

  // Re-add with updated mask.
  struct io_uring_sqe* addSqe = GetSqe(iouring);
  if (addSqe == nullptr) [[unlikely]] {
    log::error("io_uring_get_sqe failed for MOD re-add (fd # {}, events=0x{:x})", event.fd, event.eventBmp);
    return false;
  }
  const auto pollMask = static_cast<uint32_t>(event.eventBmp & EventPollMask);
  // Bump generation to invalidate any stale CQE from the old poll.
  const auto gen = ++iouring->fdGen[event.fd];
  ::io_uring_prep_poll_add(addSqe, event.fd, pollMask);
  ::io_uring_sqe_set_data64(addSqe, PackUserData(event.fd, pollMask, gen));
  // SQEs are batched and will be submitted in the next poll() cycle.
  return true;
#elifdef AERONET_LINUX
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
#ifdef AERONET_IO_URING
  auto* iouring = static_cast<IoUring*>(_pRing);

  // Bump generation so any in-flight CQEs for this fd (with the old generation) are
  // discarded during harvest.  This solves the ABA problem where the kernel recycles
  // the fd number for a new connection before stale CQEs are drained.
  if (static_cast<uint32_t>(fd) < iouring->fdGenCap) {
    ++iouring->fdGen[fd];
  }

  // Remove this fd from the deferred re-arm buffer to prevent re-arming a deleted fd.
  for (uint32_t idx = 0; idx < iouring->nbRearmPending; ++idx) {
    if (UnpackFd(iouring->rearmBuf[idx]) == fd) {
      iouring->rearmBuf[idx] = iouring->rearmBuf[--iouring->nbRearmPending];
      break;
    }
  }

  struct io_uring_sqe* sqe = GetSqe(iouring);
  if (sqe != nullptr) {
    ::io_uring_prep_cancel_fd(sqe, fd, 0);
    ::io_uring_sqe_set_data64(sqe, kCancelSentinel);
    // SQE is batched and will be submitted in the next poll() cycle.
  } else {
    log::debug("io_uring_get_sqe failed for DEL (fd # {})", fd);
  }
#elifdef AERONET_LINUX
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

// ---- submitAccept / cancelAccept ----

bool EventLoop::submitAccept(NativeHandle listenFd) {
#ifdef AERONET_IO_URING
  auto* iouring = static_cast<IoUring*>(_pRing);

  if (!EnsureFdGenCap(iouring, listenFd)) [[unlikely]] {
    log::error("Failed to grow fdGen for submitAccept (fd # {})", listenFd);
    return false;
  }

  struct io_uring_sqe* sqe = GetSqe(iouring);
  if (sqe == nullptr) [[unlikely]] {
    log::error("io_uring_get_sqe failed for submitAccept (fd # {})", listenFd);
    return false;
  }

  const auto gen = ++iouring->fdGen[listenFd];
  ::io_uring_prep_multishot_accept(sqe, listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
  ::io_uring_sqe_set_data64(sqe, PackUserData(listenFd, kAcceptMask, gen));
  // SQE is batched and will be submitted in the next poll() cycle.

  iouring->acceptListenFd = listenFd;
  return true;
#else
  // Fallback: register listen fd for read-readiness (caller must call accept() themselves).
  return add(EventFd{listenFd, EventIn});
#endif
}

void EventLoop::cancelAccept(NativeHandle listenFd) {
#ifdef AERONET_IO_URING
  auto* iouring = static_cast<IoUring*>(_pRing);

  // Bump generation to discard any in-flight accept CQEs.
  if (static_cast<uint32_t>(listenFd) < iouring->fdGenCap) {
    ++iouring->fdGen[listenFd];
  }

  struct io_uring_sqe* sqe = GetSqe(iouring);
  if (sqe != nullptr) {
    ::io_uring_prep_cancel_fd(sqe, listenFd, 0);
    ::io_uring_sqe_set_data64(sqe, kCancelSentinel);
    // Submit immediately: the caller (closeListener) closes the socket right after this call.
    // If we defer to poll(), the multishot accept still holds a kernel reference to the
    // socket file, preventing port re-bind on server restart.
    ::io_uring_submit(&iouring->ring);
    iouring->hasPendingSqes = false;
  } else {
    log::debug("io_uring_get_sqe failed for cancelAccept (fd # {})", listenFd);
  }

  iouring->acceptListenFd = -1;
#else
  del(listenFd);
#endif
}

void EventLoop::submitClose(NativeHandle fd) {
#ifdef AERONET_IO_URING
  auto* iouring = static_cast<IoUring*>(_pRing);

  // Cancel any active io_uring operations on this fd first, then close synchronously.
  // Using IORING_OP_CLOSE can delay the actual TCP FIN delivery because the kernel
  // may not fully release the socket until all io_uring references are dropped.
  // Synchronous close ensures the FIN is sent promptly.
  struct io_uring_sqe* sqe = GetSqe(iouring);
  if (sqe != nullptr) {
    ::io_uring_prep_cancel_fd(sqe, fd, 0);
    ::io_uring_sqe_set_data64(sqe, kCancelSentinel);
    ::io_uring_submit(&iouring->ring);
    iouring->hasPendingSqes = false;
  }
  // Explicit shutdown: close() alone may not send FIN if io_uring poll operations
  // still hold references to the file — the socket stays alive until all references
  // are released. shutdown(SHUT_RDWR) forces the TCP stack to send FIN immediately.
  ::shutdown(fd, SHUT_RDWR);
  ::close(fd);
#else
  BaseFd{fd};  // RAII close — immediately destroys the temporary BaseFd.
#endif
}

// ---- poll ----

std::span<const EventLoop::EventFd> EventLoop::poll() {
#ifdef AERONET_IO_URING
  auto* iouring = static_cast<IoUring*>(_pRing);

  // Re-arm: submit poll_add SQEs deferred from the PREVIOUS poll() cycle.
  // Deferred re-arm gives the caller time to consume event data before the polls
  // are re-registered, avoiding false re-triggers on still-readable fds.
  for (uint32_t idx = 0; idx < iouring->nbRearmPending; ++idx) {
    const auto rearmFd = UnpackFd(iouring->rearmBuf[idx]);
    // Skip re-arm if a mod() or del() changed the generation since this entry was recorded.
    const auto rearmIdx = static_cast<uint32_t>(rearmFd);
    if (rearmIdx >= iouring->fdGenCap || UnpackGen(iouring->rearmBuf[idx]) != iouring->fdGen[rearmFd]) {
      continue;
    }
    struct io_uring_sqe* sqe = GetSqe(iouring);
    if (sqe == nullptr) [[unlikely]] {
      log::error("io_uring_get_sqe failed during re-arm (fd # {})", rearmFd);
      continue;
    }
    const auto rearmMask = UnpackMask(iouring->rearmBuf[idx]);
    ::io_uring_prep_poll_add(sqe, rearmFd, rearmMask);
    ::io_uring_sqe_set_data64(sqe, iouring->rearmBuf[idx]);
  }
  iouring->nbRearmPending = 0;

  // Submit all batched SQEs (re-arm + add/mod/del/accept queued since last poll) in one syscall.
  if (iouring->hasPendingSqes) {
    const int submitted = ::io_uring_submit(&iouring->ring);
    if (submitted < 0) [[unlikely]] {
      log::error("io_uring_submit failed (err={}, msg={})", -submitted, SystemErrorMessage(-submitted));
      return {};
    }
    iouring->hasPendingSqes = false;
  }

  // Wait for at least one CQE, with timeout.
  struct __kernel_timespec ts{};
  ts.tv_sec = _pollTimeoutMs / 1000;
  ts.tv_nsec = static_cast<long long>(_pollTimeoutMs % 1000) * 1000000LL;

  struct io_uring_cqe* cqe = nullptr;
  const int waitRet = ::io_uring_wait_cqe_timeout(&iouring->ring, &cqe, &ts);

  if (waitRet < 0) {
    if (waitRet == -ETIME || waitRet == -EINTR) {
      // Timeout or interrupted — return empty span with non-null data (matches epoll contract).
      return {std::launder(reinterpret_cast<EventFd*>(_pEvents)), 0U};
    }
    log::error("io_uring_wait_cqe_timeout failed (timeout_ms={}, err={}, msg={})", _pollTimeoutMs, -waitRet,
               SystemErrorMessage(-waitRet));
    return {};
  }

  // Harvest all available CQEs into the EventFd output buffer.
  auto* out = std::launder(reinterpret_cast<EventFd*>(_pEvents));
  uint32_t nbReady = 0;
  uint32_t nbRearm = 0;  // Separate counter for rearmBuf (only poll CQEs, not accept).

  // Track cancelled-but-still-active fds that need re-arming (thread-exit cancellation).
  // Stack-allocated; the number of simultaneously cancelled fds is bounded by the ring size.
  constexpr uint32_t kMaxCancelledRearm = 64;
  uint64_t cancelledActive[kMaxCancelledRearm];
  uint32_t nbCancelledActive = 0;

  // Track whether multishot accept needs resubmission (IORING_CQE_F_MORE absent on the last
  // accept CQE means the kernel terminated the multishot request).
  bool acceptNeedsResubmit = false;

  unsigned head = 0;
  unsigned nbCqes = 0;
  io_uring_for_each_cqe(&iouring->ring, head, cqe) {
    const auto userData = ::io_uring_cqe_get_data64(cqe);
    ++nbCqes;

    if (userData == kCancelSentinel) {
      // Cancel/remove completion — skip.
      continue;
    }

    if (UnpackMask(userData) == kCloseMask) {
      // Async close completion — log on error and skip.
      if (cqe->res < 0) [[unlikely]] {
        log::debug("io_uring async close failed (fd # {}, err={})", UnpackFd(userData), -cqe->res);
      }
      continue;
    }

    const auto isAcceptCqe = (UnpackMask(userData) == kAcceptMask);

    if (cqe->res < 0) {
      // Poll/accept error or cancellation.
      if (cqe->res == -ECANCELED) {
        if (isAcceptCqe) {
          // Distinguish explicit cancel (cancelAccept set acceptListenFd = -1) from spurious
          // cancel (the submitting thread exited and the kernel ran io_uring_files_cancel).
          if (iouring->acceptListenFd >= 0) {
            acceptNeedsResubmit = true;
          }
          continue;
        }
        // Poll cancellation may be spurious (e.g., the submitting thread exited and the
        // kernel ran io_uring_files_cancel, cancelling polls for that task). If the fd is still
        // active (generation matches), we must re-arm it; otherwise this fd silently stops being monitored.
        const auto cancelledFd = UnpackFd(userData);
        const auto cancelledIdx = static_cast<uint32_t>(cancelledFd);
        if (cancelledIdx < iouring->fdGenCap && UnpackGen(userData) == iouring->fdGen[cancelledFd]) {
          cancelledActive[nbCancelledActive++] = userData;
        }
      } else if (isAcceptCqe && cqe->res != -EAGAIN) {
        log::warn("io_uring accept error (listen fd # {}, err={})", UnpackFd(userData), -cqe->res);
        // Multishot terminated on error — mark for resubmission.
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
          acceptNeedsResubmit = true;
        }
      }
      continue;
    }

    // Grow output + rearm buffers if needed.
    if (nbReady == _nbAllocatedEvents) {
      const uint32_t newCapacity = _nbAllocatedEvents * 2U;
      void* newEvents = std::realloc(_pEvents, static_cast<std::size_t>(newCapacity) * NativeEventSize());
      if (newEvents == nullptr) {
        log::error("Failed to reallocate io_uring event buffer from {} to {}", _nbAllocatedEvents, newCapacity);
        break;
      }
      auto* newRearm = static_cast<uint64_t*>(
          std::realloc(iouring->rearmBuf, static_cast<std::size_t>(newCapacity) * sizeof(uint64_t)));
      if (newRearm == nullptr) {
        _pEvents = newEvents;
        log::error("Failed to reallocate io_uring rearm buffer from {} to {}", _nbAllocatedEvents, newCapacity);
        break;
      }
      _pEvents = newEvents;
      iouring->rearmBuf = newRearm;
      _nbAllocatedEvents = newCapacity;
      out = std::launder(reinterpret_cast<EventFd*>(_pEvents));
    }

    const auto fd = UnpackFd(userData);

    // Discard stale CQEs whose generation does not match the current generation for this fd.
    // This handles both del()'d fds AND the ABA problem where a new connection reuses the
    // same fd number before stale CQEs from the old incarnation are drained.
    const auto fdIdx = static_cast<uint32_t>(fd);
    if (fdIdx >= iouring->fdGenCap || UnpackGen(userData) != iouring->fdGen[fd]) {
      continue;
    }

    if (isAcceptCqe) {
      // cqe->res is the newly accepted fd (already SOCK_NONBLOCK | SOCK_CLOEXEC).
      out[nbReady] = EventFd{cqe->res, EventAccept};
      ++nbReady;
      // Multishot: check if the kernel will produce more CQEs.
      if (!(cqe->flags & IORING_CQE_F_MORE)) {
        acceptNeedsResubmit = true;
      }
      // Do NOT enqueue into rearmBuf — multishot accept is self-re-arming.
    } else {
      // cqe->res contains the poll event mask for IORING_OP_POLL_ADD completions.
      const auto pollEvents = static_cast<EventBmp>(cqe->res);
      out[nbReady] = EventFd{fd, pollEvents};
      iouring->rearmBuf[nbRearm++] = userData;  // Save packed fd+mask for deferred re-arming.
      ++nbReady;
    }
  }

  ::io_uring_cq_advance(&iouring->ring, nbCqes);

  // Resubmit multishot accept if the kernel terminated it.
  if (acceptNeedsResubmit && iouring->acceptListenFd >= 0) {
    struct io_uring_sqe* sqe = GetSqe(iouring);
    if (sqe != nullptr) {
      const auto gen = ++iouring->fdGen[iouring->acceptListenFd];
      ::io_uring_prep_multishot_accept(sqe, iouring->acceptListenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      ::io_uring_sqe_set_data64(sqe, PackUserData(iouring->acceptListenFd, kAcceptMask, gen));
      // Will be submitted in the next batch at the top of poll().
    } else {
      log::error("Failed to resubmit multishot accept for listen fd # {}", iouring->acceptListenFd);
    }
  }

  // Save re-arm count for next poll() cycle (deferred re-arm).
  // rearmBuf[0..nbRearm) already contains only poll CQE entries (accept CQEs are excluded).
  iouring->nbRearmPending = nbRearm;
  // Append cancelled-but-active fds so they get re-armed.
  for (uint32_t ci = 0; ci < nbCancelledActive && iouring->nbRearmPending < _nbAllocatedEvents; ++ci) {
    iouring->rearmBuf[iouring->nbRearmPending++] = cancelledActive[ci];
  }

  return {out, nbReady};

#elifdef AERONET_LINUX
  const uint32_t capacityBeforePoll = _nbAllocatedEvents;
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
  const uint32_t capacityBeforePoll = _nbAllocatedEvents;
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

  if (nbReadyFds == 0) {
    // Timeout
    return {std::launder(reinterpret_cast<EventFd*>(_pEvents)), 0U};
  }

  const uint32_t nbReadyEvents = static_cast<uint32_t>(nbReadyFds);

#endif

#ifndef AERONET_IO_URING
  // If saturated, grow buffer for subsequent polls.
  // On Windows, buffer growth is handled in add() when registration count reaches capacity.
#ifdef AERONET_POSIX
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
#endif  // !AERONET_IO_URING
}

void EventLoop::updatePollTimeout(SysDuration pollTimeout) {
  _pollTimeoutMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(pollTimeout).count());
}

void* EventLoop::ioRing() const noexcept { return nullptr; }

NativeHandle EventLoop::splicePipeRead() const noexcept { return kInvalidHandle; }

NativeHandle EventLoop::splicePipeWrite() const noexcept { return kInvalidHandle; }

}  // namespace aeronet
