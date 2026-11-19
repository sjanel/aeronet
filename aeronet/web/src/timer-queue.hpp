#pragma once

#include <quickjs.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "aeronet/flat-hash-map.hpp"
#include "aeronet/vector.hpp"
#include "js-engine.hpp"
#include "memory-budget.hpp"

namespace aeronet::internal {

// A timer taken from its queue to run: its callback (a function, or the code of a string handler) and arguments,
// owned (the callback can clear its timer), freed with the task.
struct TimerTask {
  explicit TimerTask(JSContext* pCtx) noexcept : pCtx(pCtx) {}

  TimerTask(const TimerTask&) = delete;
  TimerTask& operator=(const TimerTask&) = delete;
  TimerTask(TimerTask&&) = delete;
  TimerTask& operator=(TimerTask&&) = delete;

  ~TimerTask() {
    JS_FreeValue(pCtx, callback);
    FreeValues(pCtx, args);
  }

  JSContext* pCtx;
  JSValue callback{JS_UNDEFINED};
  vector<JSValue> args;
};

// The timers of a page (setTimeout(), setInterval()) on its virtual clock, in ms since the start of the document,
// which jumps to each timer in turn. They hold JavaScript values of the context, freed with them. Their native memory
// is counted in the budget of the page.
class TimerQueue {
 public:
  // The timers are dropped beyond 'horizon' on the clock; an interval runs at most 'maxIntervalRuns' times.
  TimerQueue(JSContext* pCtx, MemoryBudget& memory, int64_t horizon, uint32_t maxIntervalRuns) noexcept
      : _pCtx(pCtx), _pMemory(&memory), _horizon(horizon), _maxIntervalRuns(maxIntervalRuns) {}

  TimerQueue(const TimerQueue&) = delete;
  TimerQueue& operator=(const TimerQueue&) = delete;
  TimerQueue(TimerQueue&&) = delete;
  TimerQueue& operator=(TimerQueue&&) = delete;

  ~TimerQueue();

  // Schedules 'callback' with 'args' (new references are kept) after 'delay' ms, every 'delay' ms when 'repeat'
  // (setTimeout() / setInterval()), with the HTML timer nesting clamp. Returns its id (positive), also when it is
  // dropped beyond the horizon, 0 when the page cannot hold it (nothing scheduled).
  [[nodiscard]] int32_t add(JSValueConst callback, std::span<const JSValueConst> args, int32_t delay, bool repeat);

  // Cancels the timer 'id', if pending.
  void clear(int32_t id) noexcept;

  // Whether no timer is due, cleared ones aside.
  [[nodiscard]] bool empty() const noexcept { return _queue.empty(); }

  // The due time of the earliest timer, which can be a cleared one. The queue must not be empty.
  [[nodiscard]] int64_t nextDue() const noexcept { return _queue.front().due; }

  [[nodiscard]] int64_t clock() const noexcept { return _clock; }

  // Takes the earliest timer into 'task', advancing the clock to its due time and rescheduling it if it repeats. The
  // nesting level of the timers set from now on is its own, until resetNesting(). Returns false for a cleared timer:
  // nothing is taken.
  bool takeNext(TimerTask& task);

  // A task ended with its microtasks: the timers set from now on are not nested.
  void resetNesting() noexcept { _nesting = 0; }

 private:
  struct Timer {
    JSValue callback;
    vector<JSValue> args;
    int32_t interval;  // of setInterval(), -1 for setTimeout()
    uint32_t nesting;  // nesting level (HTML timer initialization steps)
    uint32_t runs;
  };

  // A due time of a timer, in the min-heap _queue: the earliest first, then in scheduling order. A cleared timer
  // leaves its entry, skipped when reached.
  struct Slot {
    int64_t due;
    uint64_t sequence;
    int32_t id;
  };

  using Timers = flat_hash_map<int32_t, Timer>;

  // The native memory of a pending timer of 'argCount' arguments: its entry in _timers, and its slots in _queue (with
  // those of the cleared timers, which outnumber the pending ones by at most a few, see clear()).
  static constexpr std::size_t TimerMemory(std::size_t argCount) noexcept {
    return sizeof(int32_t) + sizeof(Timer) + (2 * sizeof(Slot)) + (argCount * sizeof(JSValue));
  }

  void schedule(int32_t id, int64_t due);
  void erase(Timers::iterator timerIt) noexcept;

  Timers _timers;
  vector<Slot> _queue;
  JSContext* _pCtx;
  MemoryBudget* _pMemory;
  int64_t _clock{};
  int64_t _horizon;
  uint64_t _sequence{};
  int32_t _nextId{1};
  uint32_t _nesting{};
  uint32_t _maxIntervalRuns;
};

}  // namespace aeronet::internal
