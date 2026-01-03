#pragma once

#include <atomic>
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

  // Protected by lock since callers may post from other threads.
  mutable std::mutex lock;
  vector<std::function<void(HttpServerConfig&)>> config;
  vector<std::function<void(Router&)>> router;

  std::atomic<bool> hasConfig{false};
  std::atomic<bool> hasRouter{false};
};

}  // namespace internal
}  // namespace aeronet