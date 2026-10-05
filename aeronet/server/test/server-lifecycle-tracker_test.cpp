#include "aeronet/server-lifecycle-tracker.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace aeronet {

TEST(ServerLifecycleTrackerTest, WaitUntilAllStoppedReturnsOnceAllLaunchedServersStopped) {
  ServerLifecycleTracker tracker;
  std::atomic<bool> stopRequested{false};

  tracker.notifyServerLaunched();
  tracker.notifyServerLaunched();

  std::atomic<bool> waitReturned{false};
  std::jthread waiter([&] {
    tracker.waitUntilAllStopped(stopRequested);
    waitReturned.store(true, std::memory_order_relaxed);
  });

  tracker.notifyServerStopped();
  std::this_thread::sleep_for(std::chrono::milliseconds{20});
  EXPECT_FALSE(waitReturned.load(std::memory_order_relaxed));  // one server still running

  tracker.notifyServerStopped();
  waiter.join();
  EXPECT_TRUE(waitReturned.load(std::memory_order_relaxed));
}

TEST(ServerLifecycleTrackerTest, WaitUntilAllStoppedReturnsWhenServersAlreadyStopped) {
  // Servers that stopped (e.g. drained, or failed to start) before MultiHttpServer::run() reached the wait.
  ServerLifecycleTracker tracker;
  std::atomic<bool> stopRequested{false};

  tracker.notifyServerLaunched();
  tracker.notifyServerLaunched();
  tracker.notifyServerStopped();
  tracker.notifyServerStopped();

  tracker.waitUntilAllStopped(stopRequested);
}

TEST(ServerLifecycleTrackerTest, WaitUntilAllStoppedReturnsWhenStopRequested) {
  ServerLifecycleTracker tracker;
  std::atomic<bool> stopRequested{false};
  std::promise<void> waiterStarted;
  auto waiterReady = waiterStarted.get_future();

  tracker.notifyServerLaunched();

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
