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
  // Buffer for storing packed user_data (fd + gen + mask) of multishot polls that
  // terminated unexpectedly and need resubmission (e.g., CQ overflow).
  // In normal operation this buffer stays empty.
  uint64_t* rearmBuf = nullptr;
  // Number of entries in rearmBuf that need resubmission at the start of the next poll() call.
  uint32_t nbRearmPending = 0;
  // Per-fd generation counter.  Incremented on every add/mod/del so stale CQEs from a
  // previous incarnation of the same fd number are discarded during harvest.
  // This solves the ABA problem where the kernel recycles an fd and a stale CQE passes
  // through the boolean active check.
  uint16_t* fdGen = nullptr;
  // Per-fd poll mask.  Tracks the current multishot poll event mask so that mod() can
  // compute the old user_data for io_uring_prep_poll_update without a cancel+re-add.
  uint16_t* fdMask = nullptr;
  uint32_t fdGenCap = 0;
  // Listen fd for multishot accept (-1 if not active).
  int acceptListenFd = -1;
  // Tracks whether there are un-submitted SQEs in the SQ ring.
  // When true, poll() will call io_uring_submit() before waiting.
  bool hasPendingSqes = false;
  // Lazy ring initialization: the ring is created on the first poll() call
  // so that IORING_SETUP_SINGLE_ISSUER + IORING_SETUP_DEFER_TASKRUN can be used
  // safely when the constructor runs on a different thread than the event loop.
  bool ringInitialized = false;

  // Operations buffered before the ring is created (at most a few: accept + wakeup + timer).
  struct DeferredOp {
    int fd;
    uint32_t mask;
    bool isAccept;
  };
  DeferredOp deferredOps[8]{};
  uint8_t nbDeferredOps = 0;
};

// Ensure fdGen and fdMask are large enough to hold index 'fd'. Returns false on OOM or invalid fd.
bool EnsureFdGenCap(IoUring* iouring, int fd) {
  if (fd < 0) [[unlikely]] {
    return false;
  }
  const auto idx = static_cast<uint32_t>(fd);
  if (idx < iouring->fdGenCap) {
    return true;
  }
  const uint32_t newCap = std::max(idx + 1, iouring->fdGenCap * 2);
  auto* genBuf = static_cast<uint16_t*>(std::realloc(iouring->fdGen, newCap * sizeof(uint16_t)));
  if (genBuf == nullptr) [[unlikely]] {
    return false;
  }
  iouring->fdGen = genBuf;  // Update immediately (old pointer freed by realloc).
  auto* maskBuf = static_cast<uint16_t*>(std::realloc(iouring->fdMask, newCap * sizeof(uint16_t)));
  if (maskBuf == nullptr) [[unlikely]] {
    return false;
  }
  const auto oldCap = iouring->fdGenCap;
  std::memset(genBuf + oldCap, 0, (newCap - oldCap) * sizeof(uint16_t));
  std::memset(maskBuf + oldCap, 0, (newCap - oldCap) * sizeof(uint16_t));
  iouring->fdGen = genBuf;
  iouring->fdMask = maskBuf;
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
constexpr uint32_t kAcceptMask = 0xACCE;    // Mnemonic: "ACCEpt"
constexpr uint32_t kCloseMask = 0xC105;     // Mnemonic: "CLOSe"
constexpr uint32_t kRecvMask = 0xCEC0;      // Mnemonic: "reCv"  -- async recv CQE
constexpr uint32_t kPollOnceMask = 0xB011;  // Mnemonic: "POLLone"  -- one-shot poll CQE

// Threshold for distinguishing normal poll CQEs from control CQEs (accept, close, cancel).
// Normal poll masks (POLLIN|POLLOUT|POLLERR|POLLHUP|POLLRDHUP) use small values (< 0x4000).
// Control masks use values >= kAcceptMask (0xACCE).  Comparing the full user_data against
// this threshold avoids per-CQE UnpackMask() + branch for the hot path.
constexpr uint64_t kControlCqeThreshold = static_cast<uint64_t>(kAcceptMask) << 48;

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
  // Ring creation is deferred to the first poll() call so that
  // IORING_SETUP_SINGLE_ISSUER + IORING_SETUP_DEFER_TASKRUN can be used safely
  // when the constructor runs on a different thread than the event loop.
  // All SQE operations before poll() are buffered in deferredOps[].
  _pRing = iouring;
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
#ifndef AERONET_IO_URING
  log::debug("EventLoop fd # {} opened", static_cast<intptr_t>(_baseFd.fd()));
#else
  log::debug("EventLoop allocated (ring deferred to first poll)");
#endif
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
      if (iouring->ringInitialized) {
        (void)_baseFd.release();  // Prevent double-close: io_uring_queue_exit closes the ring fd.
        ::io_uring_queue_exit(&iouring->ring);
      }
      std::free(iouring->rearmBuf);
      std::free(iouring->fdGen);
      std::free(iouring->fdMask);
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
    if (iouring->ringInitialized) {
      // Prevent BaseFd from closing the ring fd (io_uring_queue_exit does it).
      (void)_baseFd.release();
      ::io_uring_queue_exit(&iouring->ring);
    }
    std::free(iouring->rearmBuf);
    std::free(iouring->fdGen);
    std::free(iouring->fdMask);
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

  // Use multishot poll — the kernel auto-re-arms after each CQE, eliminating
  // per-event SQE submission overhead.  CQEs are generated on new I/O wakeups,
  // so the behaviour is effectively edge-triggered for socket fds.
  const auto pollMask = static_cast<uint32_t>(event.eventBmp & EventPollMask);
  // Bump generation so any stale CQEs from a previous incarnation of this fd are discarded.
  const auto gen = ++iouring->fdGen[event.fd];
  iouring->fdMask[event.fd] = static_cast<uint16_t>(pollMask);

  if (!iouring->ringInitialized) {
    // Buffer for replay on first poll().
    if (iouring->nbDeferredOps >= std::size(iouring->deferredOps)) [[unlikely]] {
      log::error("Deferred ops buffer full for ADD (fd # {})", event.fd);
      return false;
    }
    iouring->deferredOps[iouring->nbDeferredOps++] = {event.fd, pollMask, false};
    return true;
  }

  struct io_uring_sqe* sqe = GetSqe(iouring);
  if (sqe == nullptr) [[unlikely]] {
    log::error("io_uring_get_sqe failed for ADD (fd # {}, events=0x{:x}) — SQ full after submit", event.fd,
               event.eventBmp);
    return false;
  }

  ::io_uring_prep_poll_multishot(sqe, event.fd, pollMask);
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
  // For io_uring, modify the multishot poll's event mask using poll_update (single SQE)
  // instead of cancel + re-add (2 SQEs).  poll_update atomically replaces the mask
  // and user_data of an existing poll entry matched by old_user_data.
  auto* iouring = static_cast<IoUring*>(_pRing);

  if (!EnsureFdGenCap(iouring, event.fd)) [[unlikely]] {
    log::error("Failed to grow fdGen for MOD (fd # {})", event.fd);
    return false;
  }

  // Check if the fd is in the deferred re-arm buffer (its poll was terminated,
  // e.g., due to CQ overflow).  In that case, no active poll exists in the kernel
  // so poll_update would fail with ENOENT; fall back to a fresh add.
  bool wasInRearmBuf = false;
  for (uint32_t idx = 0; idx < iouring->nbRearmPending; ++idx) {
    if (UnpackFd(iouring->rearmBuf[idx]) == event.fd) {
      iouring->rearmBuf[idx] = iouring->rearmBuf[--iouring->nbRearmPending];
      wasInRearmBuf = true;
      break;
    }
  }

  const auto newMask = static_cast<uint32_t>(event.eventBmp & EventPollMask);

  if (wasInRearmBuf) {
    // No active poll in the kernel — install a fresh multishot poll.
    struct io_uring_sqe* sqe = GetSqe(iouring);
    if (sqe == nullptr) [[unlikely]] {
      log::error("io_uring_get_sqe failed for MOD re-add (fd # {}, events=0x{:x})", event.fd, event.eventBmp);
      return false;
    }
    const auto gen = ++iouring->fdGen[event.fd];
    iouring->fdMask[event.fd] = static_cast<uint16_t>(newMask);
    ::io_uring_prep_poll_multishot(sqe, event.fd, newMask);
    ::io_uring_sqe_set_data64(sqe, PackUserData(event.fd, newMask, gen));
    return true;
  }

  // Normal path: atomically update the existing multishot poll's mask in a single SQE.
  const auto oldGen = iouring->fdGen[event.fd];
  const auto oldMask = static_cast<uint32_t>(iouring->fdMask[event.fd]);
  const auto oldUserData = PackUserData(event.fd, oldMask, oldGen);

  const auto newGen = ++iouring->fdGen[event.fd];
  iouring->fdMask[event.fd] = static_cast<uint16_t>(newMask);
  const auto newUserData = PackUserData(event.fd, newMask, newGen);

  struct io_uring_sqe* sqe = GetSqe(iouring);
  if (sqe == nullptr) [[unlikely]] {
    log::error("io_uring_get_sqe failed for MOD poll_update (fd # {}, events=0x{:x})", event.fd, event.eventBmp);
    return false;
  }
  ::io_uring_prep_poll_update(sqe, oldUserData, newUserData, newMask,
                              IORING_POLL_UPDATE_EVENTS | IORING_POLL_UPDATE_USER_DATA | IORING_POLL_ADD_MULTI);
  ::io_uring_sqe_set_data64(sqe, kCancelSentinel);
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

  // No explicit cancel SQE: the generation bump above ensures stale CQEs are discarded,
  // and submitClose() (async io_uring_prep_close) will cancel the pending multishot poll
  // when the kernel processes the close operation.
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

  const auto gen = ++iouring->fdGen[listenFd];
  iouring->acceptListenFd = listenFd;

  if (!iouring->ringInitialized) {
    // Buffer for replay on first poll().
    if (iouring->nbDeferredOps >= std::size(iouring->deferredOps)) [[unlikely]] {
      log::error("Deferred ops buffer full for submitAccept (fd # {})", listenFd);
      return false;
    }
    iouring->deferredOps[iouring->nbDeferredOps++] = {listenFd, kAcceptMask, true};
    return true;
  }

  struct io_uring_sqe* sqe = GetSqe(iouring);
  if (sqe == nullptr) [[unlikely]] {
    log::error("io_uring_get_sqe failed for submitAccept (fd # {})", listenFd);
    return false;
  }

  ::io_uring_prep_multishot_accept(sqe, listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
  ::io_uring_sqe_set_data64(sqe, PackUserData(listenFd, kAcceptMask, gen));
  // SQE is batched and will be submitted in the next poll() cycle.
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

  iouring->acceptListenFd = -1;

  if (!iouring->ringInitialized) {
    // Ring was never created — just remove the deferred accept op.
    for (uint8_t idx = 0; idx < iouring->nbDeferredOps; ++idx) {
      if (iouring->deferredOps[idx].isAccept && iouring->deferredOps[idx].fd == listenFd) {
        iouring->deferredOps[idx] = iouring->deferredOps[--iouring->nbDeferredOps];
        break;
      }
    }
    return;
  }

  struct io_uring_sqe* sqe = GetSqe(iouring);
  if (sqe != nullptr) {
    ::io_uring_prep_cancel_fd(sqe, listenFd, 0);
    ::io_uring_sqe_set_data64(sqe, kCancelSentinel);
    // Submit immediately: the caller (closeListener) closes the socket right after this call.
    // If we defer to poll(), the multishot accept still holds a kernel reference to the
    // socket file, preventing port re-bind on server restart.
    // With SINGLE_ISSUER, this may fail if called from a non-issuer thread (e.g., main
    // thread after worker joined).  In that case cleanup is deferred to io_uring_queue_exit.
    const int submitRet = ::io_uring_submit(&iouring->ring);
    if (submitRet < 0) {
      log::debug("cancelAccept submit failed (err={}) — cleanup deferred to ring exit", -submitRet);
    }
    iouring->hasPendingSqes = false;
  } else {
    log::debug("io_uring_get_sqe failed for cancelAccept (fd # {})", listenFd);
  }
#else
  del(listenFd);
#endif
}

void EventLoop::submitClose(NativeHandle fd) {
#ifdef AERONET_IO_URING
  // del() has already bumped the generation counter so any in-flight CQEs from
  // the old multishot poll will be filtered by generation checks.
  // Synchronous shutdown sends FIN immediately so the remote peer knows the connection
  // is closing.  The fd release is deferred to io_uring (async close) so the fd stays
  // alive until the kernel processes the close SQE — which also cancels the pending
  // multishot poll, preventing fd reuse races where a stale poll could target a new
  // connection that got the same fd number from accept().
  ::shutdown(fd, SHUT_RDWR);
  auto* iouring = static_cast<IoUring*>(_pRing);
  struct io_uring_sqe* sqe = GetSqe(iouring);
  if (sqe != nullptr) {
    ::io_uring_prep_close(sqe, fd);
    ::io_uring_sqe_set_data64(sqe, PackUserData(fd, kCloseMask, 0));
  } else [[unlikely]] {
    // Fallback: synchronous close if SQ is somehow full even after flush.
    ::close(fd);
  }
#else
  BaseFd{fd};  // RAII close — immediately destroys the temporary BaseFd.
#endif
}

bool EventLoop::submitRecv([[maybe_unused]] NativeHandle fd, [[maybe_unused]] char* buf,
                           [[maybe_unused]] std::size_t len) {
#ifdef AERONET_IO_URING
  if (_pRing == nullptr) {
    return false;
  }
  auto* iouring = static_cast<IoUring*>(_pRing);
  if (!EnsureFdGenCap(iouring, fd)) [[unlikely]] {
    return false;
  }
  struct io_uring_sqe* sqe = GetSqe(iouring);
  if (sqe == nullptr) [[unlikely]] {
    return false;
  }
  ::io_uring_prep_recv(sqe, fd, buf, len, 0);
  ::io_uring_sqe_set_data64(sqe, PackUserData(fd, kRecvMask, iouring->fdGen[fd]));
  return true;
#else
  return false;
#endif
}

bool EventLoop::submitPollOnce([[maybe_unused]] NativeHandle fd, [[maybe_unused]] EventBmp mask) {
#ifdef AERONET_IO_URING
  if (_pRing == nullptr) {
    return false;
  }
  auto* iouring = static_cast<IoUring*>(_pRing);
  if (!EnsureFdGenCap(iouring, fd)) [[unlikely]] {
    return false;
  }
  struct io_uring_sqe* sqe = GetSqe(iouring);
  if (sqe == nullptr) [[unlikely]] {
    return false;
  }
  ::io_uring_prep_poll_add(sqe, fd, mask & EventPollMask);
  ::io_uring_sqe_set_data64(sqe, PackUserData(fd, kPollOnceMask, iouring->fdGen[fd]));
  return true;
#else
  return false;
#endif
}

// ---- poll ----

std::span<const EventLoop::EventFd> EventLoop::poll() {
#ifdef AERONET_IO_URING
  auto* iouring = static_cast<IoUring*>(_pRing);

  // Lazy ring initialization: create the ring on the first poll() call so that
  // SINGLE_ISSUER binds the submitter task to the event-loop thread.
  if (!iouring->ringInitialized) {
    const auto sqEntries = std::max(_nbAllocatedEvents, 64U);
    const auto cqEntries = std::max(_nbAllocatedEvents * 8U, 4096U);
    struct io_uring_params params{};
    // SQPOLL: kernel-side SQ poller thread.  Submissions become plain memory writes
    // to the SQ ring, no io_uring_enter() required.  This is the biggest single
    // win io_uring offers in steady-state high-throughput HTTP traffic.
    // sq_thread_idle: how long the kernel thread sleeps before requiring a wakeup.
    params.flags = IORING_SETUP_CQSIZE | IORING_SETUP_SQPOLL | IORING_SETUP_SINGLE_ISSUER;
    params.cq_entries = cqEntries;
    params.sq_thread_idle = 1000;  // ms — kernel poller sleeps after 1s of no submissions
    int initRet = ::io_uring_queue_init_params(sqEntries, &iouring->ring, &params);
    if (initRet < 0) {
      // SQPOLL may be unavailable (older kernel, no permissions). Try DEFER_TASKRUN.
      params = {};
      params.flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
      params.cq_entries = cqEntries;
      initRet = ::io_uring_queue_init_params(sqEntries, &iouring->ring, &params);
    }
    if (initRet < 0) {
      // Fallback for older kernels that lack SINGLE_ISSUER / DEFER_TASKRUN.
      params = {};
      params.flags = IORING_SETUP_CQSIZE | IORING_SETUP_COOP_TASKRUN;
      params.cq_entries = cqEntries;
      initRet = ::io_uring_queue_init_params(sqEntries, &iouring->ring, &params);
    }
    if (initRet < 0) {
      log::error("io_uring_queue_init_params failed (entries={}, err={}, msg={})", sqEntries, -initRet,
                 SystemErrorMessage(-initRet));
      return {};
    }
    _baseFd = BaseFd(iouring->ring.ring_fd);
    iouring->ringInitialized = true;

    // Replay operations that were buffered before the ring was created.
    for (uint8_t dIdx = 0; dIdx < iouring->nbDeferredOps; ++dIdx) {
      const auto& op = iouring->deferredOps[dIdx];
      struct io_uring_sqe* sqe = GetSqe(iouring);
      if (sqe == nullptr) [[unlikely]] {
        log::error("io_uring_get_sqe failed during deferred replay (fd # {})", op.fd);
        continue;
      }
      if (op.isAccept) {
        ::io_uring_prep_multishot_accept(sqe, op.fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        ::io_uring_sqe_set_data64(sqe, PackUserData(op.fd, kAcceptMask, iouring->fdGen[op.fd]));
      } else {
        ::io_uring_prep_poll_multishot(sqe, op.fd, op.mask);
        ::io_uring_sqe_set_data64(sqe, PackUserData(op.fd, op.mask, iouring->fdGen[op.fd]));
      }
    }
    iouring->nbDeferredOps = 0;
  }

  // Resubmit multishot polls that terminated during the PREVIOUS poll() cycle
  // (e.g., due to CQ overflow).  In normal operation this loop does nothing.
  for (uint32_t idx = 0; idx < iouring->nbRearmPending; ++idx) {
    const auto rearmFd = UnpackFd(iouring->rearmBuf[idx]);
    // Skip resubmit if a mod() or del() changed the generation since this entry was recorded.
    const auto rearmIdx = static_cast<uint32_t>(rearmFd);
    if (rearmIdx >= iouring->fdGenCap || UnpackGen(iouring->rearmBuf[idx]) != iouring->fdGen[rearmFd]) {
      continue;
    }
    struct io_uring_sqe* sqe = GetSqe(iouring);
    if (sqe == nullptr) [[unlikely]] {
      log::error("io_uring_get_sqe failed during poll resubmit (fd # {})", rearmFd);
      continue;
    }
    const auto rearmMask = UnpackMask(iouring->rearmBuf[idx]);
    ::io_uring_prep_poll_multishot(sqe, rearmFd, rearmMask);
    ::io_uring_sqe_set_data64(sqe, iouring->rearmBuf[idx]);
  }
  iouring->nbRearmPending = 0;

  // Submit any batched SQEs and wait for at least one CQE, in a single io_uring_enter().
  struct __kernel_timespec ts{};
  ts.tv_sec = _pollTimeoutMs / 1000;
  ts.tv_nsec = static_cast<long long>(_pollTimeoutMs % 1000) * 1000000LL;

  struct io_uring_cqe* cqe = nullptr;
  const int waitRet = ::io_uring_submit_and_wait_timeout(&iouring->ring, &cqe, 1, &ts, nullptr);
  iouring->hasPendingSqes = false;

  if (waitRet < 0) {
    if (waitRet == -ETIME || waitRet == -EINTR) {
      // Timeout or interrupted — return empty span with non-null data (matches epoll contract).
      return {std::launder(reinterpret_cast<EventFd*>(_pEvents)), 0U};
    }
    log::error("io_uring_submit_and_wait_timeout failed (timeout_ms={}, err={}, msg={})", _pollTimeoutMs, -waitRet,
               SystemErrorMessage(-waitRet));
    return {};
  }

  // Harvest all available CQEs into the EventFd output buffer.
  auto* out = std::launder(reinterpret_cast<EventFd*>(_pEvents));
  uint32_t nbReady = 0;
  uint32_t nbRearm = 0;  // Separate counter for rearmBuf (only poll CQEs, not accept).

  // Track whether multishot accept needs resubmission (IORING_CQE_F_MORE absent on the last
  // accept CQE means the kernel terminated the multishot request).
  bool acceptNeedsResubmit = false;

  unsigned head = 0;
  unsigned nbCqes = 0;
  io_uring_for_each_cqe(&iouring->ring, head, cqe) {
    const auto userData = ::io_uring_cqe_get_data64(cqe);
    ++nbCqes;

    // ---- Fast path: successful poll CQE (most common case) ----
    // Normal poll CQEs have small mask values (< kAcceptMask) in bits 48-63.
    // Check both the mask threshold and cqe->res > 0 up front.
    if (userData < kControlCqeThreshold) [[likely]] {
      if (cqe->res > 0) [[likely]] {
        const auto fd = UnpackFd(userData);
        const auto fdIdx = static_cast<uint32_t>(fd);

        // Discard stale CQEs: generation must match the current generation for this fd.
        if (fdIdx < iouring->fdGenCap && UnpackGen(userData) == iouring->fdGen[fd]) [[likely]] {
          // Grow output + rearm buffers if needed (very rare — only on initial ramp or CQ overflow).
          if (nbReady == _nbAllocatedEvents) [[unlikely]] {
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
          out[nbReady] = EventFd{fd, static_cast<EventBmp>(cqe->res)};
          ++nbReady;
          // Multishot poll: if F_MORE is absent, the kernel terminated the poll
          // (e.g., CQ overflow).  Queue for resubmission in the next poll() cycle.
          if (!(cqe->flags & IORING_CQE_F_MORE)) [[unlikely]] {
            iouring->rearmBuf[nbRearm++] = userData;
          }
        }
        continue;
      }

      // Poll error or cancellation (cqe->res <= 0).
      // ECANCELED: expected from async close cancelling the multishot poll.
      // Generation was bumped by del() before submitClose(), so this CQE's
      // generation does not match and is harmless.
      if (cqe->res != -ECANCELED) {
        log::debug("io_uring poll error (fd # {}, err={})", UnpackFd(userData), -cqe->res);
      }
      continue;
    }

    // ---- Slow path: control CQEs (cancel sentinel, async close, multishot accept, recv, poll-once) ----

    if (userData == kCancelSentinel) {
      continue;
    }

    const auto controlMask = UnpackMask(userData);

    if (controlMask == kCloseMask) {
      if (cqe->res < 0) [[unlikely]] {
        log::debug("io_uring async close failed (fd # {}, err={})", UnpackFd(userData), -cqe->res);
      }
      continue;
    }

    // ---- Async recv CQE (proactor mode) ----
    // Deliver as EventFd{fd, EventDataArrived | (poll flags), bytesAvailable=cqe->res}
    // The kernel has already written cqe->res bytes into the buffer supplied at submitRecv() time.
    // Negative res is reported as bytesAvailable=0 + EventErr/EventHup so the consumer
    // can detect failure without reading kernel error codes.
    if (controlMask == kRecvMask) {
      const auto fd = UnpackFd(userData);
      const auto fdIdx = static_cast<uint32_t>(fd);
      // Discard stale CQEs from a recycled fd.
      if (fdIdx >= iouring->fdGenCap || UnpackGen(userData) != iouring->fdGen[fd]) {
        continue;
      }
      if (nbReady == _nbAllocatedEvents) [[unlikely]] {
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
      if (cqe->res >= 0) {
        out[nbReady] = EventFd{fd, EventDataArrived, cqe->res};
        ++nbReady;
      } else if (cqe->res == -ECANCELED) {
        // recv was cancelled (e.g. on close). Generation already filtered the typical case;
        // surviving cancels mean the consumer explicitly cancelled a recv we don't track
        // separately here.  Drop silently.
      } else if (cqe->res == -EINTR) {
        // Spurious interrupt — let the consumer resubmit on next round if needed.
      } else {
        // Treat as orderly close + error indication.
        out[nbReady] = EventFd{fd, EventDataArrived | EventErr, 0};
        ++nbReady;
      }
      continue;
    }

    // ---- One-shot poll CQE (writability backpressure for async-recv connections) ----
    if (controlMask == kPollOnceMask) {
      const auto fd = UnpackFd(userData);
      const auto fdIdx = static_cast<uint32_t>(fd);
      if (fdIdx >= iouring->fdGenCap || UnpackGen(userData) != iouring->fdGen[fd]) {
        continue;
      }
      if (cqe->res < 0) {
        if (cqe->res != -ECANCELED) {
          log::debug("io_uring poll_once error (fd # {}, err={})", fd, -cqe->res);
        }
        continue;
      }
      if (nbReady == _nbAllocatedEvents) [[unlikely]] {
        const uint32_t newCapacity = _nbAllocatedEvents * 2U;
        void* newEvents = std::realloc(_pEvents, static_cast<std::size_t>(newCapacity) * NativeEventSize());
        if (newEvents == nullptr) {
          break;
        }
        auto* newRearm = static_cast<uint64_t*>(
            std::realloc(iouring->rearmBuf, static_cast<std::size_t>(newCapacity) * sizeof(uint64_t)));
        if (newRearm == nullptr) {
          _pEvents = newEvents;
          break;
        }
        _pEvents = newEvents;
        iouring->rearmBuf = newRearm;
        _nbAllocatedEvents = newCapacity;
        out = std::launder(reinterpret_cast<EventFd*>(_pEvents));
      }
      out[nbReady] = EventFd{fd, static_cast<EventBmp>(cqe->res)};
      ++nbReady;
      continue;
    }

    // Must be an accept CQE (kAcceptMask).
    if (cqe->res < 0) {
      if (cqe->res == -ECANCELED) {
        // Distinguish explicit cancel (cancelAccept set acceptListenFd = -1) from spurious cancel.
        if (iouring->acceptListenFd >= 0) {
          acceptNeedsResubmit = true;
        }
      } else if (cqe->res != -EAGAIN) {
        log::warn("io_uring accept error (listen fd # {}, err={})", UnpackFd(userData), -cqe->res);
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
          acceptNeedsResubmit = true;
        }
      }
      continue;
    }

    // Successful accept: verify generation and produce EventAccept.
    const auto fd = UnpackFd(userData);
    const auto fdIdx = static_cast<uint32_t>(fd);
    if (fdIdx >= iouring->fdGenCap || UnpackGen(userData) != iouring->fdGen[fd]) {
      // Stale accept CQE — close the orphaned fd to prevent leak.
      ::close(cqe->res);
      continue;
    }

    if (nbReady == _nbAllocatedEvents) [[unlikely]] {
      const uint32_t newCapacity = _nbAllocatedEvents * 2U;
      void* newEvents = std::realloc(_pEvents, static_cast<std::size_t>(newCapacity) * NativeEventSize());
      if (newEvents == nullptr) {
        log::error("Failed to reallocate io_uring event buffer from {} to {}", _nbAllocatedEvents, newCapacity);
        ::close(cqe->res);
        break;
      }
      auto* newRearm = static_cast<uint64_t*>(
          std::realloc(iouring->rearmBuf, static_cast<std::size_t>(newCapacity) * sizeof(uint64_t)));
      if (newRearm == nullptr) {
        _pEvents = newEvents;
        log::error("Failed to reallocate io_uring rearm buffer from {} to {}", _nbAllocatedEvents, newCapacity);
        ::close(cqe->res);
        break;
      }
      _pEvents = newEvents;
      iouring->rearmBuf = newRearm;
      _nbAllocatedEvents = newCapacity;
      out = std::launder(reinterpret_cast<EventFd*>(_pEvents));
    }

    out[nbReady] = EventFd{cqe->res, EventAccept};
    ++nbReady;
    if (!(cqe->flags & IORING_CQE_F_MORE)) {
      acceptNeedsResubmit = true;
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

  // Save terminated-multishot count for next poll() cycle (deferred resubmission).
  // rearmBuf[0..nbRearm) contains poll CQE entries whose F_MORE flag was absent
  // plus cancelled-but-active polls that need re-arming.
  iouring->nbRearmPending = nbRearm;

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

void* EventLoop::ioRing() const noexcept {
#ifdef AERONET_IO_URING
  if (_pRing != nullptr) {
    auto* iouring = static_cast<const IoUring*>(_pRing);
    if (iouring->ringInitialized) {
      return &const_cast<IoUring*>(iouring)->ring;
    }
  }
#endif
  return nullptr;
}

NativeHandle EventLoop::splicePipeRead() const noexcept { return kInvalidHandle; }

NativeHandle EventLoop::splicePipeWrite() const noexcept { return kInvalidHandle; }

}  // namespace aeronet
