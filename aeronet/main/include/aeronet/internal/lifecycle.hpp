#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>

#include "aeronet/event-fd.hpp"

namespace aeronet::internal {

struct Lifecycle {
  enum class State : uint8_t { Idle, Running, Draining, Stopping };

  Lifecycle() = default;

  Lifecycle(const Lifecycle&) = delete;

  // Explicit move so that atomics can be copied safely (we copy their values rather than moving them).
  Lifecycle(Lifecycle&& other) noexcept
      : drainDeadline(std::exchange(other.drainDeadline, {})),
        wakeupFd(std::move(other.wakeupFd)),
        state(other.state.exchange(State::Idle, std::memory_order_relaxed)),
        drainDeadlineEnabled(std::exchange(other.drainDeadlineEnabled, false)) {}

  Lifecycle& operator=(const Lifecycle&) = delete;

  Lifecycle& operator=(Lifecycle&& other) noexcept {
    if (this != &other) {
      drainDeadline = std::exchange(other.drainDeadline, {});
      wakeupFd = std::move(other.wakeupFd);
      state.store(other.state.exchange(State::Idle, std::memory_order_relaxed), std::memory_order_relaxed);
      drainDeadlineEnabled = std::exchange(other.drainDeadlineEnabled, false);
    }
    return *this;
  }

  ~Lifecycle() = default;

  void reset() noexcept {
    // Use CAS to ensure only one thread transitions to Idle and writes the non-atomic fields.
    // This avoids a data race when both SingleHttpServer::stop() and the event-loop thread call reset()
    // concurrently (e.g. during rapid stop cycles in multi-server mode).
    State expected = state.load(std::memory_order_relaxed);
    while (expected != State::Idle) {
      if (state.compare_exchange_weak(expected, State::Idle, std::memory_order_relaxed)) {
        drainDeadline = {};
        drainDeadlineEnabled = false;
        return;
      }
    }
  }

  void enterRunning() noexcept {
    state.store(State::Running, std::memory_order_relaxed);
    drainDeadlineEnabled = false;
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

  [[nodiscard]] bool hasDeadline() const noexcept { return drainDeadlineEnabled; }
  [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept { return drainDeadline; }

  // Probe status derived from state (no need for separate atomics):
  // - started: true when server has entered the event loop (state != Idle)
  // - ready: true when server is accepting normal traffic (state == Running)
  [[nodiscard]] bool started() const noexcept { return state.load(std::memory_order_relaxed) != State::Idle; }
  [[nodiscard]] bool ready() const noexcept { return state.load(std::memory_order_relaxed) == State::Running; }

  std::chrono::steady_clock::time_point drainDeadline;
  // Wakeup fd (eventfd) used to interrupt epoll_wait promptly when stop() is invoked from another thread.
  EventFd wakeupFd;
  std::atomic<State> state{State::Idle};
  bool drainDeadlineEnabled{false};
};

}  // namespace aeronet::internal
