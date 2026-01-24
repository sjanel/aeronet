#pragma once

#include <atomic>
#include <coroutine>
#include <functional>
#include <mutex>

#include "aeronet/vector.hpp"

namespace aeronet {

struct HttpServerConfig;
class Router;

namespace internal {

struct PendingUpdates {
  PendingUpdates() noexcept = default;

  PendingUpdates(const PendingUpdates& other);
  PendingUpdates& operator=(const PendingUpdates& other);

  PendingUpdates(PendingUpdates&& other) noexcept;
  PendingUpdates& operator=(PendingUpdates&& other) noexcept;

  ~PendingUpdates() = default;

  // Async callback posted from background threads to resume coroutines.
  struct AsyncCallback {
    int connectionFd;  // connection fd for O(1) hash map lookup
    std::coroutine_handle<> handle;
    std::function<void()> work;  // optional work to execute before resuming
  };

  // Protected by lock since callers may post from other threads.
  mutable std::mutex lock;
  vector<std::function<void(HttpServerConfig&)>> config;
  vector<std::function<void(Router&)>> router;
  vector<AsyncCallback> asyncCallbacks;

  std::atomic<bool> hasConfig{false};
  std::atomic<bool> hasRouter{false};
  std::atomic<bool> hasAsyncCallbacks{false};
};

}  // namespace internal
}  // namespace aeronet