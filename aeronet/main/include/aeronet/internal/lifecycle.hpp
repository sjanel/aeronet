#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>

#include "aeronet/event-fd.hpp"

namespace aeronet::internal {

struct Lifecycle {
  // TODO: Stopping state is not really needed, could be merged with Draining
  enum class State : uint8_t { Idle, Running, Draining, Stopping };

  Lifecycle() = default;

  Lifecycle(const Lifecycle&) = delete;

  // Explicit move so that atomics can be copied safely (we copy their values rather than moving them).
  Lifecycle(Lifecycle&& other) noexcept
      : drainDeadline(std::exchange(other.drainDeadline, {})),
        wakeupFd(std::move(other.wakeupFd)),
        state(other.state.exchange(State::Idle, std::memory_order_relaxed)),
        drainDeadlineEnabled(std::exchange(other.drainDeadlineEnabled, false)),
        started(other.started.load(std::memory_order_relaxed)),
        ready(other.ready.load(std::memory_order_relaxed)) {}

  Lifecycle& operator=(const Lifecycle&) = delete;

  Lifecycle& operator=(Lifecycle&& other) noexcept {
    if (this != &other) {
      drainDeadline = std::exchange(other.drainDeadline, {});
      wakeupFd = std::move(other.wakeupFd);
      state.store(other.state.exchange(State::Idle, std::memory_order_relaxed), std::memory_order_relaxed);
      drainDeadlineEnabled = std::exchange(other.drainDeadlineEnabled, false);
      started.store(other.started.load(std::memory_order_relaxed));
      ready.store(other.ready.load(std::memory_order_relaxed));
    }
    return *this;
  }

  ~Lifecycle() = default;

  void reset() noexcept {
    drainDeadline = {};
    state.store(State::Idle, std::memory_order_relaxed);
    drainDeadlineEnabled = false;
    started.store(false, std::memory_order_relaxed);
    ready.store(false, std::memory_order_relaxed);
  }

  void enterRunning() noexcept {
    state.store(State::Running, std::memory_order_relaxed);
    drainDeadlineEnabled = false;
    // Mark probes as started and ready before entering running state
    started.store(true, std::memory_order_relaxed);
    ready.store(true, std::memory_order_relaxed);
  }

  // Atomically set state to Stopping only if current state is Running.
  // Returns the previous state.
  State exchangeStopping() noexcept {
    State expected = State::Running;
    // Use strong compare_exchange to change Running -> Stopping atomically.
    if (state.compare_exchange_strong(expected, State::Stopping, std::memory_order_relaxed)) {
      drainDeadlineEnabled = false;
    }
    return expected;
  }

  void enterDraining(std::chrono::steady_clock::time_point deadline, bool enabled) noexcept {
    ready.store(false, std::memory_order_relaxed);

    drainDeadline = deadline;
    state.store(State::Draining, std::memory_order_relaxed);
    drainDeadlineEnabled = enabled;
  }

  void shrinkDeadline(std::chrono::steady_clock::time_point deadline) noexcept {
    if (!drainDeadlineEnabled || deadline < drainDeadline) {
      drainDeadline = deadline;
      drainDeadlineEnabled = true;
    }
    wakeupFd.send();
  }

  [[nodiscard]] bool isIdle() const noexcept { return state.load(std::memory_order_relaxed) == State::Idle; }
  [[nodiscard]] bool isRunning() const noexcept { return state.load(std::memory_order_relaxed) == State::Running; }
  [[nodiscard]] bool isDraining() const noexcept { return state.load(std::memory_order_relaxed) == State::Draining; }
  [[nodiscard]] bool isStopping() const noexcept { return state.load(std::memory_order_relaxed) == State::Stopping; }
  [[nodiscard]] bool isActive() const noexcept { return state.load(std::memory_order_relaxed) != State::Idle; }
  [[nodiscard]] bool acceptingConnections() const noexcept {
    return state.load(std::memory_order_relaxed) == State::Running;
  }
  [[nodiscard]] bool hasDeadline() const noexcept { return drainDeadlineEnabled; }
  [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept { return drainDeadline; }

  std::chrono::steady_clock::time_point drainDeadline;
  // Wakeup fd (eventfd) used to interrupt epoll_wait promptly when stop() is invoked from another thread.
  EventFd wakeupFd;
  std::atomic<State> state{State::Idle};
  bool drainDeadlineEnabled{false};

  // Probe flags
  std::atomic<bool> started{false};
  std::atomic<bool> ready{false};
};

}  // namespace aeronet::internal
