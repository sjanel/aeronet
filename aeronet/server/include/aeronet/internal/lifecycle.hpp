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
  enum class State : uint8_t { Idle, Starting, Running, Draining, Stopping };

  Lifecycle() = default;

  Lifecycle(const Lifecycle&) = delete;

  // Explicit move so that atomics can be copied safely (we copy their values rather than moving them).
  Lifecycle(Lifecycle&& other) noexcept
      : drainDeadline(std::exchange(other.drainDeadline, {})),
        lastLoopNs(other.lastLoopNs.exchange(0, std::memory_order_relaxed)),
        wakeupFd(std::move(other.wakeupFd)),
        state(other.state.exchange(State::Idle, std::memory_order_relaxed)),
        drainDeadlineEnabled(other.drainDeadlineEnabled.exchange(false, std::memory_order_relaxed)) {}

  Lifecycle& operator=(const Lifecycle&) = delete;

  Lifecycle& operator=(Lifecycle&& other) noexcept {
    if (this != &other) {
      drainDeadline = std::exchange(other.drainDeadline, {});
      lastLoopNs.store(other.lastLoopNs.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
      wakeupFd = std::move(other.wakeupFd);
      state.store(other.state.exchange(State::Idle, std::memory_order_relaxed), std::memory_order_relaxed);
      drainDeadlineEnabled.store(other.drainDeadlineEnabled.exchange(false, std::memory_order_relaxed),
                                 std::memory_order_relaxed);
    }
    return *this;
  }

  ~Lifecycle() = default;

  // Note on memory order: state uses acquire/release (not relaxed) because it also gates visibility of the
  // non-atomic socket/event-loop teardown performed by whichever thread transitions to Idle (see closeListener()
  // calls guarded by "if previous state was already Idle" in SingleHttpServer::stop()). A thread that observes
  // State::Idle - whether via a failed CAS here or a plain load - must see the writes that preceded the release
  // store in reset(), otherwise it can race with the event-loop thread's own in-flight teardown (caught by TSan:
  // stop()'s controller-thread closeListener() racing with runUntilStarted()'s event-loop-thread closeListener()).
  void reset() noexcept {
    // Use CAS to ensure only one thread transitions to Idle and writes the non-atomic fields.
    // This avoids a data race when both SingleHttpServer::stop() and the event-loop thread call reset()
    // concurrently (e.g. during rapid stop cycles in multi-server mode).
    for (State expected = state.load(std::memory_order_acquire); expected != State::Idle;) {
      if (state.compare_exchange_weak(expected, State::Idle, std::memory_order_release, std::memory_order_acquire)) {
        drainDeadline = {};
        drainDeadlineEnabled.store(false, std::memory_order_relaxed);
        lastLoopNs.store(0, std::memory_order_relaxed);
        state.notify_all();
        return;
      }
    }
  }

  // Atomically reserve the lifecycle for startup before creating the event-loop thread.
  // This prevents mutations from taking the pre-start direct-update path while prepareRun()
  // is inspecting the router.
  // PRECONDITION: any successful tryEnterStarting() MUST eventually be followed - from some thread -
  // by either tryEnterRunning() or reset(). exchangeStopping() blocks indefinitely on State::Starting
  // until one of those fires; a Starting state with no such follow-up call will hang stop() forever.
  // All production call sites (SingleHttpServer::run/runUntil/launchDetached) already guarantee this.
  void enterStarting() {
    State expected = State::Idle;
    if (!state.compare_exchange_strong(expected, State::Starting, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      throw std::logic_error("Lifecycle::enterStarting() called when not in Idle state");
    }
    state.notify_all();
  }

  // Transitions from Starting only, preserving a concurrent Stopping request.
  void enterRunning() {
    State expected = State::Starting;
    if (!state.compare_exchange_strong(expected, State::Running, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      throw std::logic_error("Lifecycle::enterRunning() called when not in Starting state");
    }
    drainDeadlineEnabled.store(false, std::memory_order_relaxed);
    state.notify_all();
  }

  // Blocks while the server is still starting up, then requests a stop if it reached Running/Draining.
  // This makes stop() briefly slower when called immediately after start() (bounded by however long
  // prepareRun() takes), but in exchange guarantees prepareRun() can only ever observe State::Starting.
  State exchangeStopping() noexcept {
    State current = state.load(std::memory_order_acquire);
    while (current == State::Starting) {
      state.wait(current, std::memory_order_acquire);
      current = state.load(std::memory_order_acquire);
    }
    while (current == State::Running || current == State::Draining) {
      if (state.compare_exchange_weak(current, State::Stopping, std::memory_order_acq_rel, std::memory_order_acquire)) {
        drainDeadlineEnabled.store(false, std::memory_order_relaxed);
        state.notify_all();
        return current;
      }
      // compare_exchange_weak updated `current` to the observed value; loop re-checks it.
    }
    return current;  // Idle (never started) or Stopping (someone else is already stopping it).
  }

  void enterDraining(std::chrono::steady_clock::time_point deadline, bool enabled) noexcept {
    drainDeadline = deadline;
    state.store(State::Draining, std::memory_order_release);
    drainDeadlineEnabled.store(enabled, std::memory_order_relaxed);
    state.notify_all();
  }

  void shrinkDeadline(std::chrono::steady_clock::time_point deadline) noexcept {
    if (!drainDeadlineEnabled.load(std::memory_order_relaxed) || deadline < drainDeadline) {
      drainDeadline = deadline;
      drainDeadlineEnabled.store(true, std::memory_order_relaxed);
    }
    wakeupFd.send();
  }

  [[nodiscard]] bool isIdle() const noexcept { return state.load(std::memory_order_acquire) == State::Idle; }
  [[nodiscard]] bool isRunning() const noexcept { return state.load(std::memory_order_acquire) == State::Running; }
  [[nodiscard]] bool isStarting() const noexcept { return state.load(std::memory_order_acquire) == State::Starting; }
  [[nodiscard]] bool isDraining() const noexcept { return state.load(std::memory_order_acquire) == State::Draining; }
  [[nodiscard]] bool isStopping() const noexcept { return state.load(std::memory_order_acquire) == State::Stopping; }
  [[nodiscard]] State currentState() const noexcept { return state.load(std::memory_order_acquire); }
  [[nodiscard]] bool isActive() const noexcept { return state.load(std::memory_order_acquire) != State::Idle; }

  [[nodiscard]] bool cannotBeginDraining() const noexcept {
    const State current = state.load(std::memory_order_acquire);
    return current == State::Idle || current == State::Starting || current == State::Stopping;
  }

  [[nodiscard]] bool hasDeadline() const noexcept { return drainDeadlineEnabled.load(std::memory_order_relaxed); }
  [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept { return drainDeadline; }

  // Probe status derived from state (no need for separate atomics):
  // - started: true once server initialization has completed (state != Idle/Starting)
  // - ready: true when server is accepting normal traffic (state == Running)
  [[nodiscard]] bool started() const noexcept {
    const State current = state.load(std::memory_order_acquire);
    return current != State::Idle && current != State::Starting;
  }
  [[nodiscard]] bool ready() const noexcept { return state.load(std::memory_order_acquire) == State::Running; }

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
  // reset() runs on the event-loop thread while stop() can transition the lifecycle from a controller thread.
  std::atomic<bool> drainDeadlineEnabled{false};
};

}  // namespace aeronet::internal
