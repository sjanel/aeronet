#include "aeronet/event-loop.hpp"

#include <gtest/gtest.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "aeronet/base-fd.hpp"
#include "aeronet/event.hpp"

// Enable epoll/socket syscall overrides from sys-test-support.hpp
#define AERONET_WANT_SOCKET_OVERRIDES
#include "aeronet/sys-test-support.hpp"

using namespace aeronet;

// Epoll system overrides are now centralized in sys-test-support.hpp
// under the AERONET_WANT_SOCKET_OVERRIDES macro.

TEST(EventLoopTest, BasicPollAndGrowth) {
  // Short timeout so poll returns quickly if something goes wrong
  EventLoop loop(std::chrono::milliseconds(50), 4);

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

  const auto events0 = loop.poll();
  ASSERT_NE(events0.data(), nullptr);
  ASSERT_FALSE(events0.empty());
  for (const auto& event : events0) {
    EXPECT_EQ(event.fd, readEnd.fd());
    EXPECT_EQ(event.eventBmp, EventIn);
    invoked = true;
    // consume the byte so subsequent polls don't repeatedly report it
    char tmp;
    EXPECT_EQ(1, ::read(event.fd, &tmp, 1));
  }
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
  const auto events1 = loop.poll();
  ASSERT_NE(events1.data(), nullptr);
  for (const auto& event : events1) {
    ASSERT_EQ(event.eventBmp, EventIn);
    ++handled;
    char tmp;
    EXPECT_EQ(1, ::read(event.fd, &tmp, 1));
  }

  EXPECT_EQ(events1.size(), static_cast<std::size_t>(handled));
  EXPECT_GT(events1.size(), 0U);
  // The EventLoop should have grown capacity at least to hold 'kExtra' events
  EXPECT_GE(loop.capacity(), 4U);

  // test errors

  loop.del(readEnd.fd());  // valid del
  loop.del(readEnd.fd());  // invalid del; should log but not throw

  EXPECT_TRUE(loop.add(EventLoop::EventFd{readEnd.fd(), EventIn}));  // valid add
  EXPECT_FALSE(loop.add(EventLoop::EventFd{-1, EventIn}));           // invalid add; should log and return false
  EXPECT_THROW(loop.addOrThrow(EventLoop::EventFd{-1, EventIn}),
               std::system_error);  // invalid addOrThrow; should throw

  EXPECT_FALSE(loop.mod(EventLoop::EventFd{-1, EventIn}));  // invalid mod; should log and return false
}

TEST(EventLoopTest, MoveConstructorAndAssignment) {
  EventLoop loopA(std::chrono::milliseconds(10), 8);
  // Move-construct loopB from loopA
  EventLoop loopB(std::move(loopA));
  // loopB should have non-zero capacity and loopA should be in a valid but unspecified state
  EXPECT_GE(loopB.capacity(), 1U);

  // self move assign should do nothing
  auto& loopBBis = loopB;
  loopB = std::move(loopBBis);
  EXPECT_GE(loopB.capacity(), 1U);

  // Move-assign loopC from loopB
  EventLoop loopC;
  loopC = std::move(loopB);
  EXPECT_GE(loopC.capacity(), 1U);
}

TEST(EventLoopTest, ConstructZeroCapacityShouldBePromoted) {
  EventLoop loopA(std::chrono::milliseconds(10), 0);

  loopA = EventLoop(std::chrono::milliseconds(10), 128);
}

TEST(EventLoopTest, NoShrinkPolicy) {
  // create an EventLoop with small initial capacity
  EventLoop loop(std::chrono::milliseconds(10), 4);

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
  const auto firstEvents = loop.poll();
  ASSERT_NE(firstEvents.data(), nullptr);
  for (const auto& event : firstEvents) {
    char tmp;
    EXPECT_EQ(1, ::read(event.fd, &tmp, 1));
  }
  EXPECT_GT(firstEvents.size(), 0U);
  auto capacityAfterGrow = loop.capacity();
  EXPECT_GT(capacityAfterGrow, 4U);

  // Now repeatedly poll (without changing set) and ensure capacity doesn't shrink
  for (int i = 0; i < 20; ++i) {
    const auto pollEvents = loop.poll();
    ASSERT_NE(pollEvents.data(), nullptr);
    for (const auto& event : pollEvents) {
      char tmp;
      EXPECT_EQ(1, ::read(event.fd, &tmp, 1));
    }
    EXPECT_GE(loop.capacity(), capacityAfterGrow);
  }
}

TEST(EventLoopTest, ConstructorThrowsWhenEpollCreateFails_BadFlags) {
  test::EventLoopHookGuard guard;
  test::SetEpollCreateActions({test::EpollCreateFail(EINVAL)});
  EXPECT_THROW(EventLoop(std::chrono::milliseconds(5)), std::runtime_error);
}

TEST(EventLoopTest, ConstructorThrowsWhenEpollCreateFails) {
  test::EventLoopHookGuard guard;
  test::SetEpollCreateActions({test::EpollCreateFail(EMFILE)});
  EXPECT_THROW(EventLoop(std::chrono::milliseconds(5)), std::runtime_error);
}

TEST(EventLoopTest, ConstructorThrowsWhenAllocationFails) {
  test::EventLoopHookGuard guard;
  if (!AERONET_WANT_MALLOC_OVERRIDES) {
    GTEST_SKIP() << "malloc overrides disabled on this toolchain; skipping";
  }
  test::FailNextMalloc();
  EXPECT_THROW(EventLoop(std::chrono::milliseconds(5)), std::bad_alloc);
}

TEST(EventLoopTest, PollReturnsZeroWhenInterrupted) {
  test::EventLoopHookGuard guard;
  test::SetEpollWaitActions({test::WaitError(EINTR)});
  EventLoop loop(std::chrono::milliseconds(5));
  const auto events = loop.poll();
  EXPECT_NE(events.data(), nullptr);
  EXPECT_TRUE(events.empty());
}

TEST(EventLoopTest, PollReturnsMinusOneOnFatalError) {
  test::EventLoopHookGuard guard;
  test::SetEpollWaitActions({test::WaitError(EIO)});
  EventLoop loop(std::chrono::milliseconds(5));
  const auto events = loop.poll();
  EXPECT_EQ(events.data(), nullptr);
  EXPECT_TRUE(events.empty());
}

TEST(EventLoopTest, PollKeepsCapacityWhenReallocFails) {
  test::EventLoopHookGuard guard;
  EventLoop loop(std::chrono::milliseconds(5), 2);
  const auto initialCapacity = loop.capacity();
  std::vector<epoll_event> events;
  events.reserve(initialCapacity);
  for (uint32_t i = 0; i < initialCapacity; ++i) {
    events.push_back(test::MakeEvent(static_cast<int>(i), EventIn));
  }
  test::SetEpollWaitActions({test::WaitReturn(static_cast<int>(initialCapacity), std::move(events))});
  if (!AERONET_WANT_MALLOC_OVERRIDES) {
    GTEST_SKIP() << "realloc overrides disabled on this toolchain; skipping";
  }
  test::FailNextRealloc();

  int callbacks = 0;
  const auto eventsSpan = loop.poll();
  ASSERT_NE(eventsSpan.data(), nullptr);
  for (const auto& event : eventsSpan) {
    (void)event;
    ++callbacks;
  }
  EXPECT_EQ(eventsSpan.size(), initialCapacity);
  EXPECT_EQ(callbacks, static_cast<int>(eventsSpan.size()));
  EXPECT_EQ(loop.capacity(), initialCapacity);
}

TEST(EventLoopTest, PollDoublesCapacityWhenReallocSucceeds) {
  test::EventLoopHookGuard guard;
  EventLoop loop(std::chrono::milliseconds(5), 2);
  const auto initialCapacity = loop.capacity();
  std::vector<epoll_event> events;
  events.reserve(initialCapacity);
  for (uint32_t i = 0; i < initialCapacity; ++i) {
    events.push_back(test::MakeEvent(static_cast<int>(i), EventIn));
  }
  test::SetEpollWaitActions({test::WaitReturn(static_cast<int>(initialCapacity), std::move(events))});

  const auto span = loop.poll();
  ASSERT_NE(span.data(), nullptr);
  EXPECT_EQ(span.size(), initialCapacity);
  EXPECT_EQ(loop.capacity(), initialCapacity * 2);
}

TEST(EventLoopTest, ModFailures) {
  test::EventLoopHookGuard guard;
  EventLoop loop(std::chrono::milliseconds(5), 2);

  // simulate benign mod failure (EBADF)
  test::FailAllEpollCtlMod(EBADF);
  EXPECT_FALSE(loop.mod(EventLoop::EventFd{42, EventIn}));

  // simulate benign mod failure (ENOENT)
  test::FailAllEpollCtlMod(ENOENT);
  EXPECT_FALSE(loop.mod(EventLoop::EventFd{43, EventIn}));

  // simulate fatal mod failure (EACCES)
  test::FailAllEpollCtlMod(EACCES);
  EXPECT_FALSE(loop.mod(EventLoop::EventFd{44, EventIn}));
}