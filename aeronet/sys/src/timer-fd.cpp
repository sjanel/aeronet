#include "aeronet/timer-fd.hpp"

#include "aeronet/platform.hpp"

#ifdef AERONET_LINUX
#include <sys/timerfd.h>
#include <unistd.h>
#elifdef AERONET_MACOS
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#elifdef AERONET_WINDOWS
// Windows waitable timer — headers via platform.hpp
#endif

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <stdexcept>
#include <utility>

#include "aeronet/errno-throw.hpp"
#include "aeronet/log.hpp"
#include "aeronet/timedef.hpp"

#ifdef AERONET_MACOS
#include "aeronet/socket-ops.hpp"  // SetPipeNonBlockingCloExec
#endif

namespace aeronet {

#ifdef AERONET_LINUX

namespace {
timespec ToTimespec(SysDuration dur) {
  using namespace std::chrono;
  if (dur <= SysDuration::zero()) {
    return {0, 0};
  }
  const auto ns = duration_cast<nanoseconds>(dur);
  const auto secs = duration_cast<seconds>(ns);
  const auto rem = ns - secs;
  return {static_cast<time_t>(secs.count()), static_cast<long>(rem.count())};
}
}  // namespace

// NOLINTNEXTLINE(misc-include-cleaner)
TimerFd::TimerFd() : _baseFd(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) {
  if (!_baseFd) {
    throw_errno("Unable to create a new TimerFd");
  }
  log::debug("TimerFd fd # {} opened", fd());

  // Disabled by default.
  itimerspec spec{};  // NOLINT(misc-include-cleaner)
  (void)::timerfd_settime(fd(), 0, &spec, nullptr);
}

void TimerFd::armPeriodic(SysDuration interval) const {
  itimerspec spec{};  // NOLINT(misc-include-cleaner)
  // Always compute the timespec: non-positive durations map to {0,0} which disables the timer.
  const auto ts = ToTimespec(interval);
  spec.it_interval = ts;
  spec.it_value = ts;  // start after first interval

  if (::timerfd_settime(fd(), 0, &spec, nullptr) != 0) {
    const int timerFd = fd();
    throw_errno("timerfd_settime failed (fd # {})", timerFd);
  }
}

void TimerFd::drain() const noexcept {
  while (true) {
    std::uint64_t expirations = 0;
    const auto ret = ::read(fd(), &expirations, sizeof(expirations));
    if (std::cmp_equal(ret, sizeof(expirations))) {
      // Keep draining in case multiple expirations accumulated.
      continue;
    }
    if (ret == -1) {
      const auto err = LastSystemError();
      if (err == error::kWouldBlock) {
        return;
      }
      log::error("TimerFd drain failed err={}: {}", err, SystemErrorMessage(err));
      return;
    }

    // Short read should not happen; treat as drained.
    return;
  }
}

// ---- macOS: pipe-based timer ----
#elifdef AERONET_MACOS

TimerFd::TimerFd() {
  int fds[2];
  if (::pipe(fds) != 0) {
    throw_errno("Unable to create pipe for TimerFd");
  }
  _baseFd = BaseFd(fds[0]);   // read end — registered in event loop
  _writeFd = BaseFd(fds[1]);  // write end — kqueue EVFILT_TIMER writes here

  SetPipeNonBlockingCloExec(fds[0], fds[1]);
  log::debug("TimerFd pipe read={} write={} opened", _baseFd.fd(), _writeFd.fd());
}

void TimerFd::armPeriodic(SysDuration interval) const {
  // On macOS, the EventLoop kqueue can register EVFILT_TIMER directly for the write end.
  // For simplicity, we rely on the caller (SingleHttpServer lifecycle) to also
  // register the timer pipe fd with the kqueue. The actual timer firing is achieved
  // by the reactor's poll timeout + maintenance tick pathway — consistent with how
  // the poll timeout already triggers maintenance. The pipe acts as a fallback wakeup.
  // Full EVFILT_TIMER integration is a follow-up optimisation.
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(interval).count();
  log::debug("TimerFd armPeriodic interval={}ms (macOS pipe-based, relying on poll timeout)", ms);
  (void)ms;
}

void TimerFd::drain() const noexcept {
  char buf[64];
  while (::read(_baseFd.fd(), buf, sizeof(buf)) > 0) {
  }
}

// ---- Windows: stub ----
#elifdef AERONET_WINDOWS

TimerFd::TimerFd() {
  HANDLE timer = ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
  if (timer == nullptr) {
    auto err = ::GetLastError();
    log::error("CreateWaitableTimerW failed (error={})", err);
    throw std::runtime_error("Unable to create TimerFd on Windows");
  }
  _baseFd = BaseFd(reinterpret_cast<NativeHandle>(timer), BaseFd::HandleKind::Win32Handle);
  log::debug("TimerFd Windows waitable timer created");
}

void TimerFd::armPeriodic(SysDuration interval) const {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(interval).count();
  // Convert to 100-nanosecond intervals, negative = relative time
  LARGE_INTEGER dueTime;
  dueTime.QuadPart = -static_cast<LONGLONG>(ms) * 10000LL;
  HANDLE timer = reinterpret_cast<HANDLE>(_baseFd.fd());
  if (!::SetWaitableTimer(timer, &dueTime, static_cast<LONG>(ms), nullptr, nullptr, FALSE)) {
    auto err = ::GetLastError();
    log::error("SetWaitableTimer failed (error={})", err);
  }
}

void TimerFd::drain() const noexcept {
  // Windows waitable timers don't accumulate; nothing to drain.
  log::trace("TimerFd drain (no-op on Windows)");
}

#endif

}  // namespace aeronet
