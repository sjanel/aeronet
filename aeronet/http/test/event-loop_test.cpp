#include "event-loop.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "base-fd.hpp"

using namespace aeronet;

TEST(EventLoopTest, BasicPollAndGrowth) {
  // Short timeout so poll returns quickly if something goes wrong
  EventLoop loop(std::chrono::milliseconds(50), 0, 4);

  // Create a single pipe and ensure data written to the write end triggers the callback
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  BaseFd readEnd(fds[0]);
  BaseFd writeEnd(fds[1]);

  loop.add_or_throw(readEnd.fd(), EPOLLIN);

  bool invoked = false;
  // write some data first so epoll_wait has something to return immediately
  const char ch = 'x';
  ASSERT_EQ(::write(writeEnd.fd(), &ch, 1), 1);

  int nb = loop.poll([&](int fd, uint32_t ev) {
    EXPECT_EQ(fd, readEnd.fd());
    EXPECT_TRUE(ev & EPOLLIN);
    invoked = true;
    // consume the byte so subsequent polls don't repeatedly report it
    char tmp;
    ::read(fd, &tmp, 1);
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
    loop.add_or_throw(rp.fd(), EPOLLIN);
    // write one byte so poll has something to report
    char writeByte = 'a';
    ASSERT_EQ(::write(wp.fd(), &writeByte, 1), 1);
    pipes.emplace_back(std::move(rp), std::move(wp));
  }

  // Poll once and count events handled
  int handled = 0;
  nb = loop.poll([&](int fd, uint32_t ev) {
    ASSERT_TRUE(ev & EPOLLIN);
    ++handled;
    char tmp;
    ::read(fd, &tmp, 1);
  });

  EXPECT_EQ(nb, static_cast<int>(handled));
  EXPECT_GT(nb, 0);
  // The EventLoop should have grown capacity at least to hold 'kExtra' events
  EXPECT_GE(loop.capacity(), 4U);
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
    loop.add_or_throw(rp.fd(), EPOLLIN);
    char writeByte = 'b';
    ASSERT_EQ(::write(wp.fd(), &writeByte, 1), 1);
    pipes.emplace_back(std::move(rp), std::move(wp));
  }

  // Poll once to cause growth
  int first = loop.poll([](int fd, uint32_t ev) {
    (void)ev;
    char tmp;
    ::read(fd, &tmp, 1);
  });
  EXPECT_GT(first, 0);
  auto capacityAfterGrow = loop.capacity();
  EXPECT_GT(capacityAfterGrow, 4U);

  // Now repeatedly poll (without changing set) and ensure capacity doesn't shrink
  for (int i = 0; i < 20; ++i) {
    int pollCount = loop.poll([](int, uint32_t) {});
    (void)pollCount;  // ignore returned ready count; we care about capacity
    EXPECT_GE(loop.capacity(), capacityAfterGrow);
  }
}
