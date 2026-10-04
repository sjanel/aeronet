#pragma once

namespace aeronet {

// Keeps SIGPIPE blocked in the calling thread while alive (Linux only, a no-op elsewhere).
//
// A write on a connection that the peer has reset raises SIGPIPE, which terminates the process by default. aeronet's
// own socket writes pass MSG_NOSIGNAL, but sendfile() and the writes OpenSSL makes on the socket (records, handshake,
// close_notify) cannot. A SIGPIPE raised by a write is directed to the writing thread: blocked, it stays pending
// instead of being delivered, and the outermost blocker of the thread discards it before restoring the signal mask. The
// writer still gets EPIPE.
//
// Blockers nest cheaply: only the outermost blocker of a thread makes system calls. Nothing is changed if the thread
// already blocks SIGPIPE itself - its own handling (sigwait(), ...) then sees the signal. Threads created while a
// blocker is active inherit the blocked SIGPIPE (pthread semantics): a SIGPIPE raised in them stays pending, never
// delivered. macOS sockets carry SO_NOSIGPIPE instead, and Windows has no SIGPIPE.
class SigpipeBlocker {
 public:
  // Blocks SIGPIPE in the calling thread, unless 'enable' is false (then the blocker does nothing).
  explicit SigpipeBlocker(bool enable = true) noexcept;

  SigpipeBlocker(const SigpipeBlocker&) = delete;
  SigpipeBlocker(SigpipeBlocker&&) noexcept = delete;
  SigpipeBlocker& operator=(const SigpipeBlocker&) = delete;
  SigpipeBlocker& operator=(SigpipeBlocker&&) noexcept = delete;

  // Outermost blocker: discards a pending SIGPIPE and unblocks it, if this blocker blocked it.
  ~SigpipeBlocker();

 private:
  bool _active{false};   // counted in the thread's nesting depth
  bool _unblock{false};  // blocked SIGPIPE in this thread, which did not block it before
};

}  // namespace aeronet
