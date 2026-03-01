#pragma once

#include <atomic>
#include <functional>
#include <mutex>

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>
#endif

#include "aeronet/platform.hpp"
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

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Async callback posted from background threads to resume coroutines.
  struct AsyncCallback {
    NativeHandle connectionFd;  // connection fd for O(1) hash map lookup
    std::coroutine_handle<> handle;
    std::function<void()> work;  // optional work to execute before resuming
  };
#endif

  // Protected by lock since callers may post from other threads.
  mutable std::mutex lock;
  vector<std::function<void(HttpServerConfig&)>> config;
  vector<std::function<void(Router&)>> router;
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  vector<AsyncCallback> asyncCallbacks;
#endif

  std::atomic<bool> hasConfig{false};
  std::atomic<bool> hasRouter{false};
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  std::atomic<bool> hasAsyncCallbacks{false};
#endif
};

}  // namespace internal
}  // namespace aeronet