#include "aeronet/server-lifecycle-tracker.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace aeronet {

TEST(ServerLifecycleTrackerTest, WaitUntilAnyRunningReturnsFalseWhenStopRequested) {
  ServerLifecycleTracker tracker;
  std::atomic<bool> stopRequested{false};
  std::promise<void> waiterStarted;
  auto waiterReady = waiterStarted.get_future();

  std::atomic<bool> waitResult{true};
  std::jthread waiter([&] {
    waiterStarted.set_value();
    waitResult.store(tracker.waitUntilAnyRunning(stopRequested), std::memory_order_relaxed);
  });

  waiterReady.wait();

  // Give the waiter a short window to evaluate the predicate in the false state before stop is requested.
  std::this_thread::sleep_for(std::chrono::milliseconds{20});

  stopRequested.store(true, std::memory_order_relaxed);
  tracker.notifyStopRequested();

  waiter.join();
  EXPECT_FALSE(waitResult.load(std::memory_order_relaxed));
}

TEST(ServerLifecycleTrackerTest, WaitUntilAllStoppedReturnsWhenStopRequested) {
  ServerLifecycleTracker tracker;
  std::atomic<bool> stopRequested{false};
  std::promise<void> waiterStarted;
  auto waiterReady = waiterStarted.get_future();

  tracker.notifyServerRunning();

  std::atomic<bool> waitReturned{false};
  std::jthread waiter([&] {
    waiterStarted.set_value();
    tracker.waitUntilAllStopped(stopRequested);
    waitReturned.store(true, std::memory_order_relaxed);
  });

  waiterReady.wait();

  // Keep running count non-zero and wake via stop request to exercise the second predicate branch.
  std::this_thread::sleep_for(std::chrono::milliseconds{20});
  stopRequested.store(true, std::memory_order_relaxed);
  tracker.notifyStopRequested();

  waiter.join();
  EXPECT_TRUE(waitReturned.load(std::memory_order_relaxed));
}

}  // namespace aeronet
