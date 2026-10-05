#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

#include "aeronet/event-fd.hpp"

namespace aeronet::internal {

struct Lifecycle {
  enum class State : uint8_t { Idle, Starting, Running, Draining, Stopping };

  Lifecycle() = default;

  Lifecycle(const Lifecycle&) = delete;

  // Explicit move so that atomics can be copied safely (we copy their values rather than moving them).
  Lifecycle(Lifecycle&& other) noexcept
      : drainDeadlineNs(other.drainDeadlineNs.exchange(0, std::memory_order_relaxed)),
        lastLoopNs(other.lastLoopNs.exchange(0, std::memory_order_relaxed)),
        wakeupFd(std::move(other.wakeupFd)),
        state(other.state.exchange(State::Idle, std::memory_order_relaxed)) {
    // PRECONDITION: other must be Idle (enforced by callers today - see SingleHttpServer's move ctor/assignment,
    // which check isIdle()/stop() before moving). Asserted here (not thrown) because a violation means a waiter
    // in exchangeStopping() could already be permanently stuck by the time we'd detect it - there is no safe
    // recovery, only prevention. notify_all() is defensive insurance for the (contract-violating) case where a
    // waiter is present despite the precondition.
    assert(state.load(std::memory_order_relaxed) != State::Starting);
    other.state.notify_all();
    state.notify_all();
  }

  Lifecycle& operator=(const Lifecycle&) = delete;

  Lifecycle& operator=(Lifecycle&& other) noexcept {
    if (this != &other) {
      assert(state.load(std::memory_order_relaxed) != State::Starting);  // *this* must also be Idle
      drainDeadlineNs.store(other.drainDeadlineNs.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
      lastLoopNs.store(other.lastLoopNs.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
      wakeupFd = std::move(other.wakeupFd);
      state.store(other.state.exchange(State::Idle, std::memory_order_relaxed), std::memory_order_relaxed);
      other.state.notify_all();
      state.notify_all();
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
        drainDeadlineNs.store(0, std::memory_order_relaxed);
        lastLoopNs.store(0, std::memory_order_relaxed);
        state.notify_all();
        return;
      }
    }
  }

  // Atomically reserve the lifecycle for startup before creating the event-loop thread.
  // This prevents mutations from taking the pre-start direct-update path while prepareRun()
  // is inspecting the router.
  // PRECONDITION: any successful enterStarting() MUST eventually be followed - from some thread -
  // by either enterRunning() or reset(). exchangeStopping() blocks indefinitely on State::Starting
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
    // Cleared before the transition: a beginDrain() waiting in waitWhileStarting() can set its deadline as soon as it
    // observes Running, which a store after the transition would then silently disable.
    drainDeadlineNs.store(0, std::memory_order_relaxed);
    State expected = State::Starting;
    if (!state.compare_exchange_strong(expected, State::Running, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      throw std::logic_error("Lifecycle::enterRunning() called when not in Starting state");
    }
    state.notify_all();
  }

  // Whether the calling thread is the one running the event loop (e.g. a handler).
  [[nodiscard]] bool isEventLoopThread() const noexcept {
    // Relaxed: only the event-loop thread itself can observe its own id here.
    return eventLoopThread.load(std::memory_order_relaxed) == std::this_thread::get_id();
  }

  // Blocks while the server is still starting up, then returns the observed state (never Starting).
  // Used by controller-thread requests (stop, drain) so that a request issued right after start() applies to the
  // started server instead of being lost: it is briefly slower (bounded by however long prepareRun() takes), but in
  // exchange prepareRun() can only ever observe State::Starting.
  // PRECONDITION: never called from the thread running prepareRun() while Starting (it would wait for itself).
  [[nodiscard]] State waitWhileStarting() const noexcept {
    State current = state.load(std::memory_order_acquire);
    while (current == State::Starting) {
      state.wait(current, std::memory_order_acquire);
      current = state.load(std::memory_order_acquire);
    }
    return current;
  }

  // Waits for an in-flight startup (see waitWhileStarting()), then requests a stop if it reached Running/Draining.
  State exchangeStopping() noexcept {
    State current = waitWhileStarting();
    while (current == State::Running || current == State::Draining) {
      if (state.compare_exchange_weak(current, State::Stopping, std::memory_order_acq_rel, std::memory_order_acquire)) {
        drainDeadlineNs.store(0, std::memory_order_relaxed);
        state.notify_all();
        return current;
      }
      // compare_exchange_weak updated `current` to the observed value; loop re-checks it.
    }
    return current;  // Idle (never started) or Stopping (someone else is already stopping it).
  }

  // Transitions from Running only: never overrides a concurrent stop() (Stopping), nor a server that stopped in the
  // meantime (Idle). Returns whether the transition happened.
  bool enterDraining() noexcept {
    State expected = State::Running;
    if (!state.compare_exchange_strong(expected, State::Draining, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      return false;
    }
    state.notify_all();
    return true;
  }

  // Sets the drain deadline, or moves it earlier if one is already set (a later deadline is ignored). Safe to call
  // concurrently from several controller threads while the event loop reads it.
  void shrinkDeadline(std::chrono::steady_clock::time_point deadline) noexcept {
    // 0 means 'no deadline': a steady clock reading (time since boot on Linux) plus a drain duration is never <= 0
    const std::int64_t deadlineNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count();
    assert(deadlineNs > 0);
    std::int64_t current = drainDeadlineNs.load(std::memory_order_relaxed);
    while ((current == 0 || deadlineNs < current) &&
           !drainDeadlineNs.compare_exchange_weak(current, deadlineNs, std::memory_order_relaxed)) {
      // compare_exchange_weak updated `current` to the observed value; loop re-checks it.
    }
    wakeupFd.send();
  }

  [[nodiscard]] bool isIdle() const noexcept { return state.load(std::memory_order_acquire) == State::Idle; }

  [[nodiscard]] bool isRunning() const noexcept { return state.load(std::memory_order_acquire) == State::Running; }

  [[nodiscard]] bool isStarting() const noexcept { return state.load(std::memory_order_acquire) == State::Starting; }

  [[nodiscard]] bool isDraining() const noexcept { return state.load(std::memory_order_acquire) == State::Draining; }

  [[nodiscard]] bool isStopping() const noexcept { return state.load(std::memory_order_acquire) == State::Stopping; }

  [[nodiscard]] bool isActive() const noexcept { return state.load(std::memory_order_acquire) != State::Idle; }

  [[nodiscard]] bool hasDeadline() const noexcept { return drainDeadlineNs.load(std::memory_order_relaxed) != 0; }

  [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept {
    return std::chrono::steady_clock::time_point{std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::nanoseconds{drainDeadlineNs.load(std::memory_order_relaxed)})};
  }

  // Single read of the deadline, so that a concurrent update never mixes two values.
  [[nodiscard]] bool drainDeadlineReached(std::chrono::steady_clock::time_point now) const noexcept {
    const std::int64_t deadlineNs = drainDeadlineNs.load(std::memory_order_relaxed);
    return deadlineNs != 0 &&
           std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count() >= deadlineNs;
  }

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

  // Drain deadline as steady-clock ns (see shrinkDeadline()), 0 when there is none. Atomic: written by controller
  // threads (beginDrain) while the event loop reads it.
  std::atomic<std::int64_t> drainDeadlineNs{0};
  // See loopHeartbeat(): steady-clock ns at which the event loop last reached the top of an iteration, or 0 before it
  // has started. A stuck loop stops advancing this, which is how the dedicated probe listener detects a wedge.
  std::atomic<std::int64_t> lastLoopNs{0};
  // Wakeup fd (eventfd) used to interrupt epoll_wait promptly when stop() is invoked from another thread.
  EventFd wakeupFd;
  std::atomic<State> state{State::Idle};
  // Thread running the event loop (set by SingleHttpServer::runUntilStarted() for its duration), default-constructed
  // otherwise. Not moved: moves require Idle.
  std::atomic<std::thread::id> eventLoopThread;
};

}  // namespace aeronet::internal
