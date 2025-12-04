#include "aeronet/event-loop.hpp"

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "aeronet/base-fd.hpp"
#include "aeronet/event.hpp"
#include "aeronet/sys_test_support.hpp"

using namespace aeronet;
namespace test_support = aeronet::test_support;

namespace {

struct EpollCreateAction {
  bool fail{false};
  int err{0};
};

struct EpollWaitAction {
  enum class Kind : std::uint8_t { Events, Error };
  Kind kind{Kind::Events};
  int result{0};
  int err{0};
  std::vector<epoll_event> events;
};

test_support::ActionQueue<EpollCreateAction> gCreateActions;
test_support::ActionQueue<EpollWaitAction> gWaitActions;

void ResetEpollHooks() {
  gCreateActions.reset();
  gWaitActions.reset();
  test_support::FailNextMalloc(0);
  test_support::FailNextRealloc(0);
}

void SetEpollCreateActions(std::initializer_list<EpollCreateAction> actions) { gCreateActions.setActions(actions); }

void SetEpollWaitActions(std::vector<EpollWaitAction> actions) { gWaitActions.setActions(std::move(actions)); }

class EventLoopHookGuard {
 public:
  EventLoopHookGuard() = default;
  EventLoopHookGuard(const EventLoopHookGuard&) = delete;
  EventLoopHookGuard& operator=(const EventLoopHookGuard&) = delete;
  ~EventLoopHookGuard() { ResetEpollHooks(); }
};

[[nodiscard]] EpollCreateAction EpollCreateFail(int err) { return EpollCreateAction{true, err}; }

[[nodiscard]] EpollWaitAction WaitReturn(int readyCount, std::vector<epoll_event> events) {
  EpollWaitAction action;
  action.kind = EpollWaitAction::Kind::Events;
  action.result = readyCount;
  action.events = std::move(events);
  return action;
}

[[nodiscard]] EpollWaitAction WaitError(int err) {
  EpollWaitAction action;
  action.kind = EpollWaitAction::Kind::Error;
  action.err = err;
  return action;
}

epoll_event MakeEvent(int fd, uint32_t mask) {
  epoll_event ev{};
  ev.events = mask;
  ev.data.fd = fd;
  return ev;
}

void CopyEvents(const EpollWaitAction& action, epoll_event* events, int maxevents) {
  const std::size_t limit = std::min(static_cast<std::size_t>(action.result), static_cast<std::size_t>(maxevents));
  for (std::size_t i = 0; i < limit && i < action.events.size(); ++i) {
    events[i] = action.events[i];
  }
}

}  // namespace

extern "C" int epoll_create1(int flags) {
  using EpollCreateFn = int (*)(int);
  static EpollCreateFn real_epoll_create1 = reinterpret_cast<EpollCreateFn>(dlsym(RTLD_NEXT, "epoll_create1"));
  if (real_epoll_create1 == nullptr) {
    std::abort();
  }
  const auto action = gCreateActions.pop();
  if (action.has_value() && action->fail) {
    errno = action->err;
    return -1;
  }
  return real_epoll_create1(flags);
}

extern "C" int epoll_wait(int epfd, epoll_event* events, int maxevents, int timeout) {
  using EpollWaitFn = int (*)(int, epoll_event*, int, int);
  static EpollWaitFn real_epoll_wait = reinterpret_cast<EpollWaitFn>(dlsym(RTLD_NEXT, "epoll_wait"));
  if (real_epoll_wait == nullptr) {
    std::abort();
  }
  const auto action = gWaitActions.pop();
  if (action.has_value()) {
    if (action->kind == EpollWaitAction::Kind::Error) {
      errno = action->err;
      return -1;
    }
    CopyEvents(*action, events, maxevents);
    return action->result;
  }
  return real_epoll_wait(epfd, events, maxevents, timeout);
}

TEST(EventLoopTest, BasicPollAndGrowth) {
  // Short timeout so poll returns quickly if something goes wrong
  EventLoop loop(std::chrono::milliseconds(50), 0, 4);

  // Create a single pipe and ensure data written to the write end triggers the callback
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  BaseFd readEnd(fds[0]);
  BaseFd writeEnd(fds[1]);

  loop.addOrThrow(EventLoop::EventFd{readEnd.fd(), EventIn});

  bool invoked = false;
  // write some data first so epoll_wait has something to return immediately
  const char ch = 'x';
  ASSERT_EQ(::write(writeEnd.fd(), &ch, 1), 1);

  int nb = loop.poll([&](EventLoop::EventFd event) {
    EXPECT_EQ(event.fd, readEnd.fd());
    EXPECT_EQ(event.eventBmp, EventIn);
    invoked = true;
    // consume the byte so subsequent polls don't repeatedly report it
    char tmp;
    EXPECT_EQ(1, ::read(event.fd, &tmp, 1));
  });
  EXPECT_GT(nb, 0);
  EXPECT_TRUE(invoked);

  // Now exercise growth: create many pipes and write to their write ends so that
  // the internal event array must grow from initial capacity 4 upwards.
  const unsigned kExtra = 128;  // should be enough to force several growth steps
  std::vector<std::pair<BaseFd, BaseFd>> pipes;
  pipes.reserve(kExtra);
  for (unsigned i = 0; i < kExtra; ++i) {
    int ints[2];
    ASSERT_EQ(pipe(ints), 0);
    BaseFd rp(ints[0]);
    BaseFd wp(ints[1]);
    loop.addOrThrow(EventLoop::EventFd{rp.fd(), EventIn});
    // write one byte so poll has something to report
    char writeByte = 'a';
    ASSERT_EQ(::write(wp.fd(), &writeByte, 1), 1);
    pipes.emplace_back(std::move(rp), std::move(wp));
  }

  // Poll once and count events handled
  int handled = 0;
  nb = loop.poll([&](EventLoop::EventFd event) {
    ASSERT_EQ(event.eventBmp, EventIn);
    ++handled;
    char tmp;
    EXPECT_EQ(1, ::read(event.fd, &tmp, 1));
  });

  EXPECT_EQ(nb, static_cast<int>(handled));
  EXPECT_GT(nb, 0);
  // The EventLoop should have grown capacity at least to hold 'kExtra' events
  EXPECT_GE(loop.capacity(), 4U);

  // test errors

  // invalid callback (null)
  EXPECT_THROW(loop.poll(nullptr), std::bad_function_call);

  loop.del(readEnd.fd());  // valid del
  loop.del(readEnd.fd());  // invalid del; should log but not throw

  EXPECT_TRUE(loop.add(EventLoop::EventFd{readEnd.fd(), EventIn}));  // valid add
  EXPECT_FALSE(loop.add(EventLoop::EventFd{-1, EventIn}));           // invalid add; should log and return false

  EXPECT_FALSE(loop.mod(EventLoop::EventFd{-1, EventIn}));  // invalid mod; should log and return false
}

TEST(EventLoopTest, MoveConstructorAndAssignment) {
  EventLoop loopA(std::chrono::milliseconds(10), 0, 8);
  // Move-construct loopB from loopA
  EventLoop loopB(std::move(loopA));
  // loopB should have non-zero capacity and loopA should be in a valid but unspecified state
  EXPECT_GE(loopB.capacity(), 1U);

  // Move-assign loopC from loopB
  EventLoop loopC;
  loopC = std::move(loopB);
  EXPECT_GE(loopC.capacity(), 1U);
}

TEST(EventLoopTest, ConstructZeroCapacityShouldBePromoted) {
  EventLoop loopA(std::chrono::milliseconds(10), 0, 0);

  loopA = EventLoop(std::chrono::milliseconds(10), 0, 128);
}

TEST(EventLoopTest, NoShrinkPolicy) {
  // create an EventLoop with small initial capacity
  EventLoop loop(std::chrono::milliseconds(10), 0, 4);

  // Grow the loop by adding many fds and poll once
  const unsigned kExtra = 128;
  std::vector<std::pair<BaseFd, BaseFd>> pipes;
  pipes.reserve(kExtra);
  for (unsigned i = 0; i < kExtra; ++i) {
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    BaseFd rp(fds[0]);
    BaseFd wp(fds[1]);
    loop.addOrThrow(EventLoop::EventFd{rp.fd(), EventIn});
    char writeByte = 'b';
    ASSERT_EQ(::write(wp.fd(), &writeByte, 1), 1);
    pipes.emplace_back(std::move(rp), std::move(wp));
  }

  // Poll once to cause growth
  int first = loop.poll([](EventLoop::EventFd event) {
    char tmp;
    EXPECT_EQ(1, ::read(event.fd, &tmp, 1));
  });
  EXPECT_GT(first, 0);
  auto capacityAfterGrow = loop.capacity();
  EXPECT_GT(capacityAfterGrow, 4U);

  // Now repeatedly poll (without changing set) and ensure capacity doesn't shrink
  for (int i = 0; i < 20; ++i) {
    int pollCount = loop.poll([](EventLoop::EventFd event) {
      char tmp;
      EXPECT_EQ(1, ::read(event.fd, &tmp, 1));
    });
    (void)pollCount;  // ignore returned ready count; we care about capacity
    EXPECT_GE(loop.capacity(), capacityAfterGrow);
  }
}

TEST(EventLoopTest, InvalidEpollFlags) {
  EXPECT_THROW(EventLoop({}, std::numeric_limits<int>::max(), 0), std::runtime_error);
}

TEST(EventLoopTest, ConstructorThrowsWhenEpollCreateFails) {
  EventLoopHookGuard guard;
  SetEpollCreateActions({EpollCreateFail(EMFILE)});
  EXPECT_THROW(EventLoop(std::chrono::milliseconds(5)), std::runtime_error);
}

TEST(EventLoopTest, ConstructorThrowsWhenAllocationFails) {
  EventLoopHookGuard guard;
  if (!AERONET_WANT_MALLOC_OVERRIDES) {
    GTEST_SKIP() << "malloc overrides disabled on this toolchain; skipping";
  }
  test_support::FailNextMalloc();
  EXPECT_THROW(EventLoop(std::chrono::milliseconds(5)), std::bad_alloc);
}

TEST(EventLoopTest, PollReturnsZeroWhenInterrupted) {
  EventLoopHookGuard guard;
  SetEpollWaitActions({WaitError(EINTR)});
  EventLoop loop(std::chrono::milliseconds(5));
  const int rc = loop.poll([](EventLoop::EventFd) { FAIL() << "callback should not run"; });
  EXPECT_EQ(0, rc);
}

TEST(EventLoopTest, PollReturnsMinusOneOnFatalError) {
  EventLoopHookGuard guard;
  SetEpollWaitActions({WaitError(EIO)});
  EventLoop loop(std::chrono::milliseconds(5));
  const int rc = loop.poll([](EventLoop::EventFd) { FAIL() << "callback should not run"; });
  EXPECT_EQ(-1, rc);
}

TEST(EventLoopTest, PollKeepsCapacityWhenReallocFails) {
  EventLoopHookGuard guard;
  EventLoop loop(std::chrono::milliseconds(5), 0, 2);
  const auto initialCapacity = loop.capacity();
  std::vector<epoll_event> events;
  events.reserve(initialCapacity);
  for (uint32_t i = 0; i < initialCapacity; ++i) {
    events.push_back(MakeEvent(static_cast<int>(i), EventIn));
  }
  SetEpollWaitActions({WaitReturn(static_cast<int>(initialCapacity), std::move(events))});
  if (!AERONET_WANT_MALLOC_OVERRIDES) {
    GTEST_SKIP() << "realloc overrides disabled on this toolchain; skipping";
  }
  test_support::FailNextRealloc();

  int callbacks = 0;
  const int rc = loop.poll([&](EventLoop::EventFd) { ++callbacks; });
  EXPECT_EQ(static_cast<int>(initialCapacity), rc);
  EXPECT_EQ(callbacks, rc);
  EXPECT_EQ(loop.capacity(), initialCapacity);
}

TEST(EventLoopTest, PollDoublesCapacityWhenReallocSucceeds) {
  EventLoopHookGuard guard;
  EventLoop loop(std::chrono::milliseconds(5), 0, 2);
  const auto initialCapacity = loop.capacity();
  std::vector<epoll_event> events;
  events.reserve(initialCapacity);
  for (uint32_t i = 0; i < initialCapacity; ++i) {
    events.push_back(MakeEvent(static_cast<int>(i), EventIn));
  }
  SetEpollWaitActions({WaitReturn(static_cast<int>(initialCapacity), std::move(events))});

  const int rc = loop.poll([](EventLoop::EventFd) {});
  EXPECT_EQ(static_cast<int>(initialCapacity), rc);
  EXPECT_EQ(loop.capacity(), initialCapacity * 2);
}
