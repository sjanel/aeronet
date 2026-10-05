#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace aeronet {

// Counts the MultiHttpServer workers whose thread is not done yet: each one is counted from its launch on the
// controller thread (not once its event loop runs), so that waiting for all of them to stop also covers the ones that
// stop - or fail to start - before the wait begins.
class ServerLifecycleTracker {
 public:
  void clear() {
    std::scoped_lock lock(_mutex);
    _running = 0;
  }

  void notifyServerLaunched() {
    std::scoped_lock lock(_mutex);
    ++_running;
    _cv.notify_all();
  }

  void notifyServerStopped() noexcept {
    std::scoped_lock lock(_mutex);
    assert(_running > 0);
    --_running;
    _cv.notify_all();
  }

  void notifyStopRequested() { _cv.notify_all(); }

  void waitUntilAllStopped(const std::atomic<bool>& stopRequested) {
    std::unique_lock lock(_mutex);
    _cv.wait(lock, [&] { return _running == 0 || stopRequested.load(std::memory_order_relaxed); });
  }

 private:
  std::mutex _mutex;
  std::condition_variable _cv;
  int64_t _running{0};
};

}  // namespace aeronet
