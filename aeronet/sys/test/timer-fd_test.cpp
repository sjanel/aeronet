#include "aeronet/timer-fd.hpp"

#include <gtest/gtest.h>
#include <sys/timerfd.h>
#include <sys/types.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "aeronet/base-fd.hpp"
#include "aeronet/timedef.hpp"

// Enable read override from sys-test-support.hpp so we can deterministically
// drive TimerFd::drain() without relying on a real timerfd.
#define AERONET_WANT_READ_WRITE_OVERRIDES
#include "aeronet/sys-test-support.hpp"

using namespace aeronet;

namespace {

struct TimerfdCreateAction {
  enum class Kind : std::uint8_t { ReturnFd, Error };
  Kind kind{Kind::ReturnFd};
  int fd{BaseFd::kClosedFd};
  int err{0};
};

[[nodiscard]] TimerfdCreateAction CreateReturnFd(int fd) {
  TimerfdCreateAction act;
  act.kind = TimerfdCreateAction::Kind::ReturnFd;
  act.fd = fd;
  return act;
}

[[nodiscard]] TimerfdCreateAction CreateError(int err) {
  TimerfdCreateAction act;
  act.kind = TimerfdCreateAction::Kind::Error;
  act.err = err;
  return act;
}

struct TimerfdSettimeAction {
  enum class Kind : std::uint8_t { Success, Error };
  Kind kind{Kind::Success};
  int err{0};
};

[[nodiscard]] TimerfdSettimeAction SettimeSuccess() {
  return TimerfdSettimeAction{TimerfdSettimeAction::Kind::Success, 0};
}

[[nodiscard]] TimerfdSettimeAction SettimeError(int err) {
  TimerfdSettimeAction act;
  act.kind = TimerfdSettimeAction::Kind::Error;
  act.err = err;
  return act;
}

struct TimerfdCreateCall {
  int clockId{-1};
  int flags{0};
};

struct TimerfdSettimeCall {
  int fd{-1};
  int flags{0};
  itimerspec spec{};  // NOLINT(misc-include-cleaner)
};

class TimerfdOverrideState {
 public:
  void reset() {
    std::scoped_lock<std::mutex> lock(_mutex);
    _createActions.clear();
    _createCalls.clear();
    _settimeActions.reset();
    _settimeCalls.clear();
  }

  void pushCreateAction(TimerfdCreateAction action) {
    std::scoped_lock<std::mutex> lock(_mutex);
    _createActions.emplace_back(std::move(action));
  }

  std::optional<TimerfdCreateAction> popCreateAction() {
    std::scoped_lock<std::mutex> lock(_mutex);
    if (_createActions.empty()) {
      return std::nullopt;
    }
    auto front = std::move(_createActions.front());
    _createActions.pop_front();
    return front;
  }

  void recordCreateCall(int clockId, int flags) {
    std::scoped_lock<std::mutex> lock(_mutex);
    _createCalls.push_back(TimerfdCreateCall{clockId, flags});
  }

  [[nodiscard]] std::vector<TimerfdCreateCall> createCalls() const {
    std::scoped_lock<std::mutex> lock(_mutex);
    return _createCalls;
  }

  void setSettimeActions(int fd, std::initializer_list<TimerfdSettimeAction> actions) {
    _settimeActions.setActions(fd, actions);
  }

  std::optional<TimerfdSettimeAction> popSettimeAction(int fd) { return _settimeActions.pop(fd); }

  void recordSettimeCall(int fd, int flags, const itimerspec& spec) {
    std::scoped_lock<std::mutex> lock(_mutex);
    TimerfdSettimeCall call;
    call.fd = fd;
    call.flags = flags;
    call.spec = spec;
    _settimeCalls.push_back(call);
  }

  [[nodiscard]] std::vector<TimerfdSettimeCall> settimeCalls() const {
    std::scoped_lock<std::mutex> lock(_mutex);
    return _settimeCalls;
  }

 private:
  mutable std::mutex _mutex;
  std::deque<TimerfdCreateAction> _createActions;
  std::vector<TimerfdCreateCall> _createCalls;

  aeronet::test::KeyedActionQueue<int, TimerfdSettimeAction> _settimeActions;
  std::vector<TimerfdSettimeCall> _settimeCalls;
};

TimerfdOverrideState gTimerfd;

class TimerfdOverrideGuard {
 public:
  TimerfdOverrideGuard() = default;
  TimerfdOverrideGuard(const TimerfdOverrideGuard&) = delete;
  TimerfdOverrideGuard& operator=(const TimerfdOverrideGuard&) = delete;
  ~TimerfdOverrideGuard() { gTimerfd.reset(); }
};

using TimerfdCreateFn = int (*)(int, int);
using TimerfdSettimeFn = int (*)(int, int, const itimerspec*, itimerspec*);

TimerfdCreateFn ResolveRealTimerfdCreate() {
  static TimerfdCreateFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<TimerfdCreateFn>("timerfd_create");
  return fn;
}

[[nodiscard]] std::string SysErrorWhat(const std::system_error& err) { return err.what(); }

}  // namespace

// NOLINTNEXTLINE
extern "C" int timerfd_create(int clockid, int flags) {
  gTimerfd.recordCreateCall(clockid, flags);

  if (auto action = gTimerfd.popCreateAction()) {
    if (action->kind == TimerfdCreateAction::Kind::ReturnFd) {
      return action->fd;
    }
    errno = action->err;
    return -1;
  }

  // If no test action provided, fall back to real syscall.
  return ResolveRealTimerfdCreate()(clockid, flags);
}

// NOLINTNEXTLINE
extern "C" int timerfd_settime(int fd, int flags, const itimerspec* new_value, itimerspec* old_value) {
  itimerspec specCopy{};
  if (new_value != nullptr) {
    specCopy = *new_value;
  }
  gTimerfd.recordSettimeCall(fd, flags, specCopy);

  if (auto action = gTimerfd.popSettimeAction(fd)) {
    if (action->kind == TimerfdSettimeAction::Kind::Success) {
      if (old_value != nullptr) {
        std::memset(old_value, 0, sizeof(*old_value));
      }
      return 0;
    }
    errno = action->err;
    return -1;
  }

  // If the test didn't configure behavior, default to success to avoid calling
  // the real syscall on a non-timerfd fd (we often use memfd fakes).
  if (old_value != nullptr) {
    std::memset(old_value, 0, sizeof(*old_value));
  }
  return 0;
}

TEST(TimerFdTest, DefaultCtorThrowsWhenCreateFails) {
  TimerfdOverrideGuard guard;
  gTimerfd.pushCreateAction(CreateError(EMFILE));

  try {
    TimerFd timer;
    (void)timer;
    FAIL() << "Expected std::system_error";
  } catch (const std::system_error& e) {
    EXPECT_EQ(e.code().value(), EMFILE);
    const auto what = SysErrorWhat(e);
    EXPECT_NE(what.find("Unable to create a new TimerFd"), std::string::npos);
  }
}

TEST(TimerFdTest, DefaultCtorCreatesAndDisablesTimer) {
  TimerfdOverrideGuard guard;

  const int fakeFd = test::CreateMemfd("aeronet-timerfd-fake");
  ASSERT_GE(fakeFd, 0);

  gTimerfd.pushCreateAction(CreateReturnFd(fakeFd));
  gTimerfd.setSettimeActions(fakeFd, {SettimeSuccess()});

  TimerFd timer;
  EXPECT_EQ(timer.fd(), fakeFd);

  const auto calls = gTimerfd.createCalls();
  ASSERT_EQ(calls.size(), 1U);
  EXPECT_EQ(calls[0].clockId, CLOCK_MONOTONIC);  // NOLINT(misc-include-cleaner)
  EXPECT_EQ(calls[0].flags, (TFD_NONBLOCK | TFD_CLOEXEC));

  const auto setCalls = gTimerfd.settimeCalls();
  ASSERT_FALSE(setCalls.empty());
  // The ctor disables the timer by default.
  EXPECT_EQ(setCalls[0].fd, fakeFd);
  EXPECT_EQ(setCalls[0].spec.it_interval.tv_sec, 0);
  EXPECT_EQ(setCalls[0].spec.it_interval.tv_nsec, 0);
  EXPECT_EQ(setCalls[0].spec.it_value.tv_sec, 0);
  EXPECT_EQ(setCalls[0].spec.it_value.tv_nsec, 0);
}

TEST(TimerFdTest, ArmPeriodicDisablesOnNonPositiveInterval) {
  TimerfdOverrideGuard guard;

  const int fakeFd = test::CreateMemfd("aeronet-timerfd-disable");
  ASSERT_GE(fakeFd, 0);

  gTimerfd.pushCreateAction(CreateReturnFd(fakeFd));
  // ctor disable + armPeriodic call
  gTimerfd.setSettimeActions(fakeFd, {SettimeSuccess(), SettimeSuccess()});

  TimerFd timer;
  timer.armPeriodic(SysDuration::zero());

  const auto setCalls = gTimerfd.settimeCalls();
  ASSERT_GE(setCalls.size(), 2U);
  const auto& armCall = setCalls[1];
  EXPECT_EQ(armCall.spec.it_interval.tv_sec, 0);
  EXPECT_EQ(armCall.spec.it_interval.tv_nsec, 0);
  EXPECT_EQ(armCall.spec.it_value.tv_sec, 0);
  EXPECT_EQ(armCall.spec.it_value.tv_nsec, 0);
}

TEST(TimerFdTest, ArmPeriodicSetsExpectedTimespec) {
  using namespace std::chrono;

  TimerfdOverrideGuard guard;

  const int fakeFd = test::CreateMemfd("aeronet-timerfd-arm");
  ASSERT_GE(fakeFd, 0);

  gTimerfd.pushCreateAction(CreateReturnFd(fakeFd));
  // ctor disable + armPeriodic call
  gTimerfd.setSettimeActions(fakeFd, {SettimeSuccess(), SettimeSuccess()});

  TimerFd timer;
  timer.armPeriodic(milliseconds(1500));

  const auto setCalls = gTimerfd.settimeCalls();
  ASSERT_GE(setCalls.size(), 2U);
  const auto& armCall = setCalls[1];

  EXPECT_EQ(armCall.spec.it_interval.tv_sec, 1);
  EXPECT_EQ(armCall.spec.it_interval.tv_nsec, 500000000L);
  EXPECT_EQ(armCall.spec.it_value.tv_sec, 1);
  EXPECT_EQ(armCall.spec.it_value.tv_nsec, 500000000L);
}

TEST(TimerFdTest, ArmPeriodicThrowsOnSettimeFailure) {
  using namespace std::chrono;

  TimerfdOverrideGuard guard;

  const int fakeFd = test::CreateMemfd("aeronet-timerfd-settime-fail");
  ASSERT_GE(fakeFd, 0);

  gTimerfd.pushCreateAction(CreateReturnFd(fakeFd));
  // ctor disable succeeds, arm fails
  gTimerfd.setSettimeActions(fakeFd, {SettimeSuccess(), SettimeError(EINVAL)});

  TimerFd timer;
  try {
    timer.armPeriodic(milliseconds(10));
    FAIL() << "Expected std::system_error";
  } catch (const std::system_error& e) {
    EXPECT_EQ(e.code().value(), EINVAL);
    const auto what = SysErrorWhat(e);
    EXPECT_NE(what.find("timerfd_settime failed"), std::string::npos);
    EXPECT_NE(what.find(std::to_string(fakeFd)), std::string::npos);
  }
}

TEST(TimerFdTest, DrainReturnsOnEagain) {
  TimerfdOverrideGuard guard;

  const int fakeFd = test::CreateMemfd("aeronet-timerfd-drain-eagain");
  ASSERT_GE(fakeFd, 0);

  gTimerfd.pushCreateAction(CreateReturnFd(fakeFd));
  gTimerfd.setSettimeActions(fakeFd, {SettimeSuccess()});

  TimerFd timer;

  test::SetReadActions(fakeFd, {{-1, EAGAIN}});
  timer.drain();
}

TEST(TimerFdTest, DrainDrainsMultipleThenStopsOnEagain) {
  TimerfdOverrideGuard guard;

  const int fakeFd = test::CreateMemfd("aeronet-timerfd-drain-multi");
  ASSERT_GE(fakeFd, 0);

  gTimerfd.pushCreateAction(CreateReturnFd(fakeFd));
  gTimerfd.setSettimeActions(fakeFd, {SettimeSuccess()});

  TimerFd timer;

  test::SetReadActions(fakeFd, {
                                   {static_cast<ssize_t>(sizeof(std::uint64_t)), 0},
                                   {static_cast<ssize_t>(sizeof(std::uint64_t)), 0},
                                   {-1, EAGAIN},
                               });

  timer.drain();
}

TEST(TimerFdTest, DrainReturnsOnShortRead) {
  TimerfdOverrideGuard guard;

  const int fakeFd = test::CreateMemfd("aeronet-timerfd-drain-short");
  ASSERT_GE(fakeFd, 0);

  gTimerfd.pushCreateAction(CreateReturnFd(fakeFd));
  gTimerfd.setSettimeActions(fakeFd, {SettimeSuccess()});

  TimerFd timer;

  test::SetReadActions(fakeFd, {
                                   {1, 0},  // short read
                                   // If drain loops incorrectly, the next action forces a failure.
                                   {-1, EBADF},
                               });

  timer.drain();
}

TEST(TimerFdTest, DrainHandlesNonEagainError) {
  TimerfdOverrideGuard guard;

  const int fakeFd = test::CreateMemfd("aeronet-timerfd-drain-err");
  ASSERT_GE(fakeFd, 0);

  gTimerfd.pushCreateAction(CreateReturnFd(fakeFd));
  gTimerfd.setSettimeActions(fakeFd, {SettimeSuccess()});

  TimerFd timer;

  test::SetReadActions(fakeFd, {{-1, EBADF}});
  timer.drain();
}
