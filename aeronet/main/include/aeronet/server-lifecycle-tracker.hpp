#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace aeronet {

class ServerLifecycleTracker {
 public:
  void clear() {
    std::scoped_lock lock(_mutex);
    _running = 0;
  }

  void notifyServerRunning() {
    std::scoped_lock lock(_mutex);
    ++_running;
    _cv.notify_all();
  }

  void notifyServerStopped() {
    std::scoped_lock lock(_mutex);
    if (_running > 0) {
      --_running;
    }
    _cv.notify_all();
  }

  void notifyStopRequested() { _cv.notify_all(); }

  bool waitUntilAnyRunning(const std::atomic<bool>& stopRequested) {
    std::unique_lock lock(_mutex);
    _cv.wait(lock, [&] { return _running > 0 || stopRequested.load(std::memory_order_relaxed); });
    return _running > 0;
  }

  void waitUntilAllStopped(const std::atomic<bool>& stopRequested) {
    std::unique_lock lock(_mutex);
    _cv.wait(lock, [&] { return _running == 0 || stopRequested.load(std::memory_order_relaxed); });
  }

 private:
  std::mutex _mutex;
  std::condition_variable _cv;
  std::size_t _running{0};
};

}  // namespace aeronet
