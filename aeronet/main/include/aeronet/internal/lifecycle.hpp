#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>

#include "event-fd.hpp"

namespace aeronet::internal {

struct Lifecycle {
  enum class State : uint8_t { Idle, Running, Draining, Stopping };

  Lifecycle() = default;

  Lifecycle(const Lifecycle&) = delete;

  // Explicit move so that atomics can be copied safely (we copy their values rather than moving them).
  Lifecycle(Lifecycle&& other) noexcept
      : drainDeadline(std::exchange(other.drainDeadline, {})),
        wakeupFd(std::move(other.wakeupFd)),
        state(std::exchange(other.state, State::Idle)),
        drainDeadlineEnabled(std::exchange(other.drainDeadlineEnabled, false)),
        started(other.started.load(std::memory_order_relaxed)),
        ready(other.ready.load(std::memory_order_relaxed)) {}

  Lifecycle& operator=(const Lifecycle&) = delete;

  Lifecycle& operator=(Lifecycle&& other) noexcept {
    if (this != &other) {
      drainDeadline = std::exchange(other.drainDeadline, {});
      wakeupFd = std::move(other.wakeupFd);
      state = std::exchange(other.state, State::Idle);
      drainDeadlineEnabled = std::exchange(other.drainDeadlineEnabled, false);
      started.store(other.started.load(std::memory_order_relaxed));
      ready.store(other.ready.load(std::memory_order_relaxed));
    }
    return *this;
  }

  ~Lifecycle() = default;

  void reset() noexcept {
    drainDeadline = {};
    state = State::Idle;
    drainDeadlineEnabled = false;
    started.store(false, std::memory_order_relaxed);
    ready.store(false, std::memory_order_relaxed);
  }

  void enterRunning() noexcept {
    state = State::Running;
    drainDeadlineEnabled = false;
    // Mark probes as started and ready before entering running state
    started.store(true, std::memory_order_relaxed);
    ready.store(true, std::memory_order_relaxed);
  }

  void enterStopping() noexcept {
    state = State::Stopping;
    drainDeadlineEnabled = false;
    // Trigger wakeup to break any blocking epoll_wait quickly.
    wakeupFd.send();
  }

  void enterDraining(std::chrono::steady_clock::time_point deadline, bool enabled) noexcept {
    ready.store(false, std::memory_order_relaxed);

    drainDeadline = deadline;
    state = State::Draining;
    drainDeadlineEnabled = enabled;
    wakeupFd.send();
  }

  void shrinkDeadline(std::chrono::steady_clock::time_point deadline) noexcept {
    if (!drainDeadlineEnabled || deadline < drainDeadline) {
      drainDeadline = deadline;
      drainDeadlineEnabled = true;
    }
    wakeupFd.send();
  }

  [[nodiscard]] bool isIdle() const noexcept { return state == State::Idle; }
  [[nodiscard]] bool isRunning() const noexcept { return state == State::Running; }
  [[nodiscard]] bool isDraining() const noexcept { return state == State::Draining; }
  [[nodiscard]] bool isStopping() const noexcept { return state == State::Stopping; }
  [[nodiscard]] bool isActive() const noexcept { return state != State::Idle; }
  [[nodiscard]] bool acceptingConnections() const noexcept { return state == State::Running; }
  [[nodiscard]] bool hasDeadline() const noexcept { return drainDeadlineEnabled; }
  [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept { return drainDeadline; }

  std::chrono::steady_clock::time_point drainDeadline;
  // Wakeup fd (eventfd) used to interrupt epoll_wait promptly when stop() is invoked from another thread.
  EventFd wakeupFd;
  State state{State::Idle};
  bool drainDeadlineEnabled{false};

  // Probe flags
  std::atomic<bool> started{false};
  std::atomic<bool> ready{false};
};

}  // namespace aeronet::internal
