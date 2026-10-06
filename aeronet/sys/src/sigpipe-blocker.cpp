#include "aeronet/sigpipe-blocker.hpp"

#include <signal.h>  // NOLINT(modernize-deprecated-headers) POSIX API: sigset_t, pthread_sigmask, sigtimedwait

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <ctime>

namespace aeronet {

namespace {

// Number of active blockers in the current thread.
thread_local uint32_t sigpipeBlockerDepth = 0;

sigset_t SigpipeSet() noexcept {  // NOLINT(misc-include-cleaner)
  sigset_t sigpipeSet;
  ::sigemptyset(&sigpipeSet);
  ::sigaddset(&sigpipeSet, SIGPIPE);
  return sigpipeSet;
}

}  // namespace

SigpipeBlocker::SigpipeBlocker(bool enable) noexcept : _active(enable) {
  if (!enable || sigpipeBlockerDepth++ != 0) {
    return;
  }
  const sigset_t sigpipeSet = SigpipeSet();
  sigset_t previousMask;
  [[maybe_unused]] const int err = ::pthread_sigmask(SIG_BLOCK, &sigpipeSet, &previousMask);
  assert(err == 0);  // only fails on invalid arguments
  _unblock = sigismember(&previousMask, SIGPIPE) == 0;
}

SigpipeBlocker::~SigpipeBlocker() {
  if (!_active || --sigpipeBlockerDepth != 0 || !_unblock) {
    return;
  }
  const sigset_t sigpipeSet = SigpipeSet();
  // Discard the SIGPIPE raised while blocked (standard signals do not queue: at most one is pending), so that
  // unblocking does not deliver it.
  static constexpr timespec kNoWait{};
  int ret;
  do {
    ret = ::sigtimedwait(&sigpipeSet, nullptr, &kNoWait);
  } while (ret == -1 && errno == EINTR);
  [[maybe_unused]] const int err = ::pthread_sigmask(SIG_UNBLOCK, &sigpipeSet, nullptr);
  assert(err == 0);  // only fails on invalid arguments
}

}  // namespace aeronet
