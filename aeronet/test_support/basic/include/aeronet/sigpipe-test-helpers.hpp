#pragma once

#ifdef AERONET_POSIX
#include <signal.h>  // NOLINT(modernize-deprecated-headers) POSIX API: sigset_t, pthread_sigmask, sigpending

namespace aeronet::test {

// Default SIGPIPE disposition (terminate the process) for the scope: a SIGPIPE reaching the process then fails the
// test, even if a helper ignored it beforehand (TlsClient ignores SIGPIPE process-wide).
class DefaultSigpipeScope {
 public:
  DefaultSigpipeScope() : _previous(::signal(SIGPIPE, SIG_DFL)) {}

  DefaultSigpipeScope(const DefaultSigpipeScope&) = delete;
  DefaultSigpipeScope(DefaultSigpipeScope&&) = delete;
  DefaultSigpipeScope& operator=(const DefaultSigpipeScope&) = delete;
  DefaultSigpipeScope& operator=(DefaultSigpipeScope&&) = delete;

  ~DefaultSigpipeScope() { ::signal(SIGPIPE, _previous); }

 private:
  void (*_previous)(int);
};

// Whether SIGPIPE is blocked in the calling thread.
inline bool IsSigpipeBlocked() {
  sigset_t mask;
  ::pthread_sigmask(SIG_BLOCK, nullptr, &mask);
  return (sigismember)(&mask, SIGPIPE) == 1;  // parenthesized: the function, not the macOS macro (sign-conversion)
}

// Whether a SIGPIPE is pending for the calling thread (or the process).
inline bool IsSigpipePending() {
  sigset_t pending;
  ::sigpending(&pending);
  return (sigismember)(&pending, SIGPIPE) == 1;  // parenthesized: the function, not the macOS macro
}

}  // namespace aeronet::test

#endif
