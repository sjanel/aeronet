#include "aeronet/timer-fd.hpp"

#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <utility>

#include "aeronet/errno-throw.hpp"
#include "aeronet/log.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

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

TimerFd::TimerFd() : _baseFd(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) {
  if (!_baseFd) {
    throw_errno("Unable to create a new TimerFd");
  }
  log::debug("TimerFd fd # {} opened", fd());

  // Disabled by default.
  itimerspec spec{};
  (void)::timerfd_settime(fd(), 0, &spec, nullptr);
}

void TimerFd::armPeriodic(SysDuration interval) const {
  itimerspec spec{};
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
      const auto err = errno;
      if (err == EAGAIN) {
        return;
      }
      log::error("TimerFd drain failed err={}: {}", err, std::strerror(err));
      return;
    }

    // Short read should not happen; treat as drained.
    return;
  }
}

}  // namespace aeronet
