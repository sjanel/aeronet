#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>

#include "aeronet/event-fd.hpp"

namespace aeronet::internal {

// Steady clock reading expressed in nanoseconds since its epoch, as a plain integer suitable for a lock-free atomic.
[[nodiscard]] inline std::int64_t SteadyNowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct Lifecycle {
  enum class State : uint8_t { Idle, Running, Draining, Stopping };

  Lifecycle() = default;

  Lifecycle(const Lifecycle&) = delete;

  // Explicit move so that atomics can be copied safely (we copy their values rather than moving them).
  Lifecycle(Lifecycle&& other) noexcept
      : drainDeadline(std::exchange(other.drainDeadline, {})),
        lastLoopNs(other.lastLoopNs.exchange(0, std::memory_order_relaxed)),
        wakeupFd(std::move(other.wakeupFd)),
        state(other.state.exchange(State::Idle, std::memory_order_relaxed)),
        drainDeadlineEnabled(std::exchange(other.drainDeadlineEnabled, false)) {}

  Lifecycle& operator=(const Lifecycle&) = delete;

  Lifecycle& operator=(Lifecycle&& other) noexcept {
    if (this != &other) {
      drainDeadline = std::exchange(other.drainDeadline, {});
      lastLoopNs.store(other.lastLoopNs.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
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
    for (State expected = state.load(std::memory_order_relaxed); expected != State::Idle;) {
      if (state.compare_exchange_weak(expected, State::Idle, std::memory_order_relaxed)) {
        drainDeadline = {};
        drainDeadlineEnabled = false;
        lastLoopNs.store(0, std::memory_order_relaxed);
        return;
      }
    }
  }

  void enterRunning() noexcept {
    state.store(State::Running, std::memory_order_relaxed);
    drainDeadlineEnabled = false;
  }

  // Atomically set state to Stopping if current state is Running or Draining.
  // Returns the previous state.
  State exchangeStopping() noexcept {
    State expected = State::Running;
    // Use strong compare_exchange to change Running -> Stopping atomically.
    if (state.compare_exchange_strong(expected, State::Stopping, std::memory_order_relaxed)) {
      drainDeadlineEnabled = false;
      return expected;
    }
    // Also handle Draining -> Stopping (e.g. stop() called after beginDrain()).
    expected = State::Draining;
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

  // Loop heartbeat used by a dedicated probe listener to detect a wedged event loop.
  // Published once per iteration at the top of the loop (see SingleHttpServer::eventLoop): if the loop is stuck
  // inside a request handler (or otherwise not polling), this timestamp stops advancing and goes stale. It reuses
  // the loop's already-computed 'now', so it costs a single relaxed store and is published unconditionally.
  // Note: an idle loop only refreshes it once per poll cycle, so a healthy loop can look up to
  // pollInterval * pollIntervalMaxFactor stale - callers must keep the staleness threshold well above that.
  void loopHeartbeat(std::chrono::steady_clock::time_point now) noexcept {
    lastLoopNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(),
                     std::memory_order_relaxed);
  }

  // Returns true if the loop published a heartbeat within thresholdNs (i.e. it is progressing), or has not started
  // yet (lastLoopNs == 0). nowNs is a caller-provided SteadyNowNs() reading so a batch check reuses one clock read.
  [[nodiscard]] bool loopHealthy(std::int64_t nowNs, std::int64_t thresholdNs) const noexcept {
    const auto last = lastLoopNs.load(std::memory_order_relaxed);
    return last == 0 || (nowNs - last) <= thresholdNs;
  }

  std::chrono::steady_clock::time_point drainDeadline;
  // See loopHeartbeat(): steady-clock ns at which the event loop last reached the top of an iteration, or 0 before it
  // has started. A stuck loop stops advancing this, which is how the dedicated probe listener detects a wedge.
  std::atomic<std::int64_t> lastLoopNs{0};
  // Wakeup fd (eventfd) used to interrupt epoll_wait promptly when stop() is invoked from another thread.
  EventFd wakeupFd;
  std::atomic<State> state{State::Idle};
  bool drainDeadlineEnabled{false};
};

}  // namespace aeronet::internal
