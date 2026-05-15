#include "aeronet/internal/keep-alive-deadline-queue.hpp"

#include <gtest/gtest.h>

#include <chrono>

#include "aeronet/connection-state.hpp"
#include "aeronet/native-handle.hpp"

namespace aeronet::internal {

namespace {

constexpr auto kNoIndex = ConnectionState::kNoKeepAliveDeadlineIndex;

}  // namespace

TEST(KeepAliveDeadlineQueue, PopsEarliestDeadlineFirst) {
  KeepAliveDeadlineQueue queue;
  ConnectionState stateA;
  ConnectionState stateB;
  ConnectionState stateC;
  const auto base = std::chrono::steady_clock::now();

  queue.upsert(stateA, NativeHandle{10}, base + std::chrono::milliseconds{30});
  queue.upsert(stateB, NativeHandle{11}, base + std::chrono::milliseconds{10});
  queue.upsert(stateC, NativeHandle{12}, base + std::chrono::milliseconds{20});

  EXPECT_EQ(queue.top().fd, NativeHandle{11});

  const auto first = queue.pop();
  EXPECT_EQ(first.fd, NativeHandle{11});
  EXPECT_EQ(stateB.keepAliveDeadlineIndex, kNoIndex);

  const auto second = queue.pop();
  EXPECT_EQ(second.fd, NativeHandle{12});
  EXPECT_EQ(stateC.keepAliveDeadlineIndex, kNoIndex);

  const auto third = queue.pop();
  EXPECT_EQ(third.fd, NativeHandle{10});
  EXPECT_EQ(stateA.keepAliveDeadlineIndex, kNoIndex);
  EXPECT_TRUE(queue.empty());
}

TEST(KeepAliveDeadlineQueue, UpsertMovesExistingEntryBothDirections) {
  KeepAliveDeadlineQueue queue;
  ConnectionState stateA;
  ConnectionState stateB;
  ConnectionState stateC;
  const auto base = std::chrono::steady_clock::now();

  queue.upsert(stateA, NativeHandle{20}, base + std::chrono::milliseconds{10});
  queue.upsert(stateB, NativeHandle{21}, base + std::chrono::milliseconds{20});
  queue.upsert(stateC, NativeHandle{22}, base + std::chrono::milliseconds{30});

  queue.upsert(stateA, NativeHandle{20}, base + std::chrono::milliseconds{40});
  EXPECT_EQ(queue.top().fd, NativeHandle{21});

  queue.upsert(stateC, NativeHandle{22}, base + std::chrono::milliseconds{5});
  EXPECT_EQ(queue.top().fd, NativeHandle{22});

  EXPECT_EQ(queue.pop().fd, NativeHandle{22});
  EXPECT_EQ(queue.pop().fd, NativeHandle{21});
  EXPECT_EQ(queue.pop().fd, NativeHandle{20});
}

TEST(KeepAliveDeadlineQueue, RemoveMiddleEntryPreservesHeap) {
  KeepAliveDeadlineQueue queue;
  ConnectionState stateA;
  ConnectionState stateB;
  ConnectionState stateC;
  ConnectionState stateD;
  const auto base = std::chrono::steady_clock::now();

  queue.upsert(stateA, NativeHandle{30}, base + std::chrono::milliseconds{40});
  queue.upsert(stateB, NativeHandle{31}, base + std::chrono::milliseconds{10});
  queue.upsert(stateC, NativeHandle{32}, base + std::chrono::milliseconds{30});
  queue.upsert(stateD, NativeHandle{33}, base + std::chrono::milliseconds{20});

  queue.remove(stateD);
  EXPECT_EQ(stateD.keepAliveDeadlineIndex, kNoIndex);

  EXPECT_EQ(queue.pop().fd, NativeHandle{31});
  EXPECT_EQ(queue.pop().fd, NativeHandle{32});
  EXPECT_EQ(queue.pop().fd, NativeHandle{30});
}

TEST(KeepAliveDeadlineQueue, ClearUnschedulesAllStates) {
  KeepAliveDeadlineQueue queue;
  ConnectionState stateA;
  ConnectionState stateB;
  const auto base = std::chrono::steady_clock::now();

  queue.upsert(stateA, NativeHandle{40}, base + std::chrono::milliseconds{10});
  queue.upsert(stateB, NativeHandle{41}, base + std::chrono::milliseconds{20});
  queue.clear();

  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(stateA.keepAliveDeadlineIndex, kNoIndex);
  EXPECT_EQ(stateB.keepAliveDeadlineIndex, kNoIndex);
}

}  // namespace aeronet::internal
