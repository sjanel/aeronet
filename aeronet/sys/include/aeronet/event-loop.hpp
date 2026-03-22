#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#ifdef AERONET_LINUX
#include <sys/epoll.h>
#endif

#include "aeronet/adaptive-poll-timeout.hpp"
#include "aeronet/base-fd.hpp"
#include "aeronet/event.hpp"
#include "aeronet/native-handle.hpp"

namespace aeronet {

#ifdef AERONET_LINUX
namespace detail {
// Padding helper to mirror epoll_event layout inside EventFd exactly.
// Sizes are derived from epoll_event via offsetof/sizeof — no arch-specific guards needed.
// EpollPad<0> is an empty type; [[no_unique_address]] ensures it occupies zero bytes.
template <std::size_t N>
struct alignas(1) EpollPad {
  uint8_t _[N];
};
template <>
struct EpollPad<0> {};
}  // namespace detail
#endif

// Thin RAII wrapper over a platform event-notification mechanism
// (epoll on Linux, kqueue on macOS, WSAPoll on Windows).
//
// Design notes:
//  * Inherits from BaseFd to reuse unified close()/logging + move semantics.
//  * Event buffer starts with kInitialCapacity (64). Rationale:
//      - Large enough to avoid immediate reallocations for small / moderate servers.
//      - Keeps stack/heap churn low in the common path while not over‑allocating.
//    On saturation (returned events == current capacity) the capacity is doubled.
//    This exponential growth yields amortized O(1) reallocation behaviour and
//    quickly reaches an adequate size for higher concurrency (64 -> 128 -> 256 ...).
//  * We do not shrink the buffer; poll cost is independent of capacity and
//    keeping the memory avoids oscillations under fluctuating load.
//  * add()/mod()/del() return success/failure and log details on failure; caller
//    can decide policy (e.g., drop connection / abort).
class EventLoop {
 public:
  static constexpr uint32_t kInitialCapacity = 64;

  struct EventFd {
    constexpr EventFd(NativeHandle fd, EventBmp eventBmp) : eventBmp(eventBmp), fd(fd) {}
#if !defined(AERONET_LINUX) || defined(AERONET_IO_URING)
    EventFd(NativeHandle fd, EventBmp eventBmp, int32_t bytesAvailable)
        : eventBmp(eventBmp), fd(fd), bytesAvailable(bytesAvailable) {}
#endif

    EventBmp eventBmp;
#if defined(AERONET_LINUX) && !defined(AERONET_IO_URING)
    // Layout mirrors epoll_event exactly; sizes derived from epoll_event via offsetof/sizeof.
    // The static_assert in event-loop.cpp validates the result at compile time.
    [[no_unique_address]] detail::EpollPad<offsetof(epoll_event, data.fd) - sizeof(EventBmp)> _pre_fd_pad;
    NativeHandle fd;
    [[no_unique_address]] detail::EpollPad<sizeof(epoll_event) - offsetof(epoll_event, data.fd) - sizeof(NativeHandle)>
        _post_fd_pad;
#else
    NativeHandle fd;
#ifdef AERONET_POSIX
    // Number of bytes ready in the connection's recv buffer when EventDataArrived is set.
    // Negative values reserved for future use; zero means orderly EOF when EventDataArrived is set.
    int32_t bytesAvailable{0};
#endif
#endif
  };

  // Default constructor - creates an empty EventLoop.
  EventLoop() noexcept = default;

  // Construct an EventLoop with a fixed or adaptive poll timeout policy.
  // Parameters:
  //   timeoutPolicy    -> validated poll timeout policy; validate() is called here.
  //   initialCapacity  -> starting number of event slots reserved in the internal buffer.
  //                       Must be > 0. Values <= 0 are promoted to 1. A value of 64 is a good
  //                       balance for small/medium workloads: it fits easily in cache (< 1 KB)
  //                       yet avoids immediate reallocations. Buffer grows by doubling whenever
  //                       a poll returns exactly capacity() events. It never shrinks.
  explicit EventLoop(PollTimeoutPolicy timeoutPolicy, uint32_t initialCapacity = kInitialCapacity);

  EventLoop(const EventLoop&) = delete;
  EventLoop(EventLoop&& rhs) noexcept;
  EventLoop& operator=(const EventLoop&) = delete;
  EventLoop& operator=(EventLoop&& rhs) noexcept;

  ~EventLoop();

  // Register fd with given events.
  // On error, throws std::system_error.
  void addOrThrow(EventFd event);

  // Register fd with given events.
  // Returns true on success, false on failure (logged).
  [[nodiscard]] bool add(EventFd event);

  // Modify fd with given events.
  // Returns true on success, false on failure (logged).
  [[nodiscard]] bool mod(EventFd event);

  // Delete fd from monitoring.
  // Log on error.
  void del(NativeHandle fd);

  // Submit a completion-based accept on the given listen socket.
  // On io_uring: submits IORING_OP_ACCEPT (multishot). Accepted fds are delivered as
  //   EventFd{newFd, EventAccept} in subsequent poll() calls.
  // On other backends: equivalent to add(EventFd{listenFd, EventIn}) — caller must
  //   still call accept() when the listen fd becomes readable.
  // Returns true on success.
  [[nodiscard]] bool submitAccept(NativeHandle listenFd);

  // Cancel a previously submitted multishot accept on the given listen socket.
  // On io_uring: cancels the outstanding IORING_OP_ACCEPT SQE.
  // On other backends: equivalent to del(listenFd).
  void cancelAccept(NativeHandle listenFd);

  // Close an fd asynchronously.
  // On io_uring: queues IORING_OP_CLOSE (non-blocking, batched with next poll cycle).
  //   The caller must have released ownership of the fd (e.g. via Connection::release())
  //   to prevent double-close from RAII destructors.
  // On other backends: calls ::close(fd) synchronously.
  void submitClose(NativeHandle fd);

  // Submit an asynchronous recv (io_uring proactor mode) for the given fd into the user-provided
  // buffer.  When data arrives, the next poll() returns an EventFd with `EventDataArrived` set
  // and `bytesAvailable` equal to the number of bytes the kernel wrote into `buf`
  // (0 means orderly EOF, negative reserved). The caller must keep `buf` valid until the
  // corresponding CQE is harvested.
  // Returns true if the SQE was queued successfully.  Only available on io_uring backend;
  // other backends return false (caller must use add()/poll-based reads).
  [[nodiscard]] bool submitRecv(NativeHandle fd, char* buf, std::size_t len);

  // Submit an asynchronous send (io_uring proactor mode) of [buf, buf+len) on the given fd.
  // The kernel keeps the operation pending until at least one byte can be sent (no EAGAIN
  // round-trips).  Completion is delivered as an EventFd with `EventSendComplete` set and
  // `bytesAvailable` equal to the number of bytes consumed (negative values are -errno).
  // Short sends are normal: the caller resubmits the remainder.
  // moreToCome sets MSG_MORE — use it when another send for the same fd follows immediately,
  // so the kernel coalesces the partial segment (equivalent to TCP_CORK, without syscalls).
  // The buffer must remain valid and unmodified until the completion is harvested.
  // Returns true if the SQE was queued.  On non-io_uring backends, returns false.
  [[nodiscard]] bool submitSend(NativeHandle fd, const char* buf, std::size_t len, bool moreToCome);

  // Submit a single-shot poll (io_uring proactor mode) for the given fd and event mask.
  // Used by callers that registered the fd via submitRecv() (and thus do NOT have a
  // multishot poll active) to wait for writability when a write would block.
  // The CQE is delivered as a normal poll EventFd in the next poll() and the wait is
  // automatically deregistered.
  // Returns true on success.  On non-io_uring backends, returns false.
  [[nodiscard]] bool submitPollOnce(NativeHandle fd, EventBmp mask);

  // Cancel every pending ring operation (io_uring IORING_ASYNC_CANCEL_ANY).
  // Must be called from the current polling thread, before it stops polling: it queues the
  // cancel SQE, and the caller must keep calling poll() until the terminal CQEs (recv/send
  // cancellations, accept/poll teardown) have been drained.  Without this, an abandoned
  // ring's multishot accept stays armed while the kernel context dies asynchronously,
  // silently consuming connections from a still-open listener.
  // No-op on non-io_uring backends and when the ring was never initialized.
  void cancelAllOps();

  // Notify the event loop that the polling thread is (re)starting.  Must be called from the
  // thread that will subsequently call poll().
  // On io_uring: the ring is bound to its first submitter thread (SINGLE_ISSUER,
  // DEFER_TASKRUN, registered ring fd).  When a server is restarted on a new thread while
  // reusing its EventLoop (listener kept bound), the ring is torn down and lazily re-created
  // on the new thread, re-arming the tracked kernel state (multishot accept + multishot
  // polls).  No-op on other backends and when the thread is unchanged.
  void prepareForLoopThread();

  // Polls for ready events up to the poll timeout.
  //
  // Returns a span over an internal, reusable buffer (no allocations on the hot path).
  //
  // Semantics:
  //  - On success: returns a non-empty span of ready events.
  //  - On timeout or when interrupted by a signal (error::kInterrupted): returns an empty span
  //    with non-null data() pointer.
  //  - On unrecoverable poll failure (already logged): returns an empty span
  //    with nullptr data() pointer.
  [[nodiscard]] std::span<const EventFd> poll();

  // Current allocated capacity (number of event slots available without reallocation).
  [[nodiscard]] uint32_t capacity() const noexcept { return _nbAllocatedEvents; }

  // Current effective poll timeout in milliseconds.
  [[nodiscard]] int currentPollTimeoutMs() const noexcept { return _timeout.pollTimeoutMs(); }

  // Update the fixed or adaptive poll timeout policy.
  void updatePollTimeoutPolicy(PollTimeoutPolicy timeoutPolicy) { _timeout = AdaptivePollTimeout(timeoutPolicy); }

 private:
  uint32_t _nbAllocatedEvents = 0;
  AdaptivePollTimeout _timeout;
  BaseFd _baseFd;
  void* _pEvents = nullptr;
#ifdef AERONET_IO_URING
  void* _pRing = nullptr;  // heap-allocated struct io_uring for event notifications (poll/accept/cancel/close)
#endif
#ifdef AERONET_WINDOWS
  void* _pPollFds = nullptr;   // WSAPOLLFD registration array for WSAPoll()
  uint32_t _nbRegistered = 0;  // number of fds currently registered
#endif
};

}  // namespace aeronet
