#include "aeronet/internal/pending-updates.hpp"

#include <atomic>
#include <utility>

namespace aeronet::internal {

PendingUpdates::PendingUpdates(const PendingUpdates& other) : config(other.config), router(other.router) {
  hasConfig.store(other.hasConfig.load(std::memory_order_relaxed), std::memory_order_relaxed);
  hasRouter.store(other.hasRouter.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

PendingUpdates& PendingUpdates::operator=(const PendingUpdates& other) {
  if (this != &other) [[likely]] {
    config = other.config;
    router = other.router;
    hasConfig.store(other.hasConfig.load(std::memory_order_relaxed), std::memory_order_relaxed);
    hasRouter.store(other.hasRouter.load(std::memory_order_relaxed), std::memory_order_relaxed);
  }
  return *this;
}

PendingUpdates::PendingUpdates(PendingUpdates&& other) noexcept
    : config(std::move(other.config)), router(std::move(other.router)) {
  hasConfig.store(other.hasConfig.exchange(false, std::memory_order_acq_rel), std::memory_order_release);
  hasRouter.store(other.hasRouter.exchange(false, std::memory_order_acq_rel), std::memory_order_release);
}

PendingUpdates& PendingUpdates::operator=(PendingUpdates&& other) noexcept {
  if (this != &other) [[likely]] {
    config = std::move(other.config);
    router = std::move(other.router);
    hasConfig.store(other.hasConfig.exchange(false, std::memory_order_acq_rel), std::memory_order_release);
    hasRouter.store(other.hasRouter.exchange(false, std::memory_order_acq_rel), std::memory_order_release);
  }
  return *this;
}

}  // namespace aeronet::internal