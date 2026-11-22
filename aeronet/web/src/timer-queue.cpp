#include "timer-queue.hpp"

#include <quickjs.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include "aeronet/log.hpp"
#include "aeronet/vector.hpp"
#include "js-engine.hpp"
#include "memory-budget.hpp"

namespace aeronet::internal {

namespace {

// The cleared timers leave their slot in the queue, until they outnumber the pending ones by this much.
constexpr std::size_t kMaxStaleSlots = 64;

// The capacity of the queue for its first timers, doubled when full.
constexpr uint32_t kFirstSlots = 8;

// HTML timer initialization steps: beyond 5 nested levels, a timer waits at least 4 ms.
constexpr uint32_t kMaxNestingWithoutClamp = 5;
constexpr int32_t kNestedMinDelay = 4;

// The min-heap order of the timers.
constexpr auto kLater = [](const auto& lhs, const auto& rhs) noexcept {
  return lhs.due > rhs.due || (lhs.due == rhs.due && lhs.sequence > rhs.sequence);
};

}  // namespace

TimerQueue::~TimerQueue() {
  for (const Timers::value_type& entry : _timers) {
    JS_FreeValue(_pCtx, entry.second.callback);
    FreeValues(_pCtx, entry.second.args);
  }
}

int32_t TimerQueue::add(JSValueConst callback, std::span<const JSValueConst> args, int32_t delay, bool repeat) {
  int32_t id = 0;
  do {  // the ids of the pending timers stay unique once they wrapped around
    id = _nextId;
    _nextId = _nextId == std::numeric_limits<int32_t>::max() ? 1 : _nextId + 1;
  } while (_timers.contains(id));

  const uint32_t nesting = _nesting + 1;

  delay = std::max(delay, 0);

  if (nesting > kMaxNestingWithoutClamp) {
    delay = std::max(delay, kNestedMinDelay);
  }

  if (repeat && _maxIntervalRuns == 0) {
    return id;
  }

  const int64_t due = _clock + delay;
  if (due > _horizon) {
    log::debug("[js] timer of {} ms beyond the horizon of the virtual clock: dropped", delay);
    return id;
  }

  const std::size_t memory = TimerMemory(args.size());
  if (!_pMemory->canHold(memory)) {
    return 0;
  }

  // Every allocation comes first: nothing fails once the timer is added.
  vector<JSValue> values;
  values.reserve(static_cast<vector<JSValue>::size_type>(args.size()));
  if (_queue.size() == _queue.capacity()) {
    _queue.reserve(std::max<decltype(_queue.size())>(2 * _queue.size(), kFirstSlots));
  }

  Timer& timer =
      _timers.emplace(id, Timer{JS_UNDEFINED, std::move(values), repeat ? delay : -1, nesting, 0}).first->second;
  timer.callback = JS_DupValue(_pCtx, callback);

  for (const JSValueConst arg : args) {
    timer.args.push_back(JS_DupValue(_pCtx, arg));  // within the reserved capacity
  }

  schedule(id, due);  // within the reserved capacity

  _pMemory->charge(memory);
  return id;
}

void TimerQueue::schedule(int32_t id, int64_t due) {
  _queue.emplace_back(due, _sequence, id);
  ++_sequence;
  std::ranges::push_heap(_queue, kLater);
}

void TimerQueue::erase(Timers::iterator timerIt) noexcept {
  _pMemory->release(TimerMemory(timerIt->second.args.size()));
  JS_FreeValue(_pCtx, timerIt->second.callback);
  FreeValues(_pCtx, timerIt->second.args);
  _timers.erase(timerIt);
}

void TimerQueue::clear(int32_t id) noexcept {
  const auto timerIt = _timers.find(id);
  if (timerIt == _timers.end()) {
    return;
  }

  erase(timerIt);

  // Each slot is a pending timer or a cleared one: those of the cleared timers are dropped once they are too many
  // (clearTimeout(setTimeout(...)) in a loop).
  if (_queue.size() > (2 * _timers.size()) + kMaxStaleSlots) {
    const auto stale = std::ranges::remove_if(_queue, [this](const Slot& slot) { return !_timers.contains(slot.id); });
    _queue.erase(stale.begin(), stale.end());
    std::ranges::make_heap(_queue, kLater);
  }
}

bool TimerQueue::takeNext(TimerTask& task) {
  std::ranges::pop_heap(_queue, kLater);

  const Slot slot = _queue.pop_back_val();

  const auto timerIt = _timers.find(slot.id);
  if (timerIt == _timers.end()) {
    return false;
  }

  _clock = std::max(_clock, slot.due);

  Timer& timer = timerIt->second;
  _nesting = timer.nesting;

  if (timer.interval < 0 || ++timer.runs >= _maxIntervalRuns) {
    // The last run: the values move to the task.
    _pMemory->release(TimerMemory(timer.args.size()));
    task.callback = timer.callback;
    task.args = std::move(timer.args);
    _timers.erase(timerIt);
    return true;
  }

  task.args.reserve(timer.args.size());
  task.callback = JS_DupValue(_pCtx, timer.callback);

  for (const JSValue arg : timer.args) {
    task.args.push_back(JS_DupValue(_pCtx, arg));  // within the reserved capacity
  }

  // Each repetition of an interval is one more nesting level.
  int32_t delay = timer.interval;
  if (++timer.nesting > kMaxNestingWithoutClamp) {
    delay = std::max(delay, kNestedMinDelay);
  }

  if (_clock + delay > _horizon) {
    erase(timerIt);
  } else {
    schedule(slot.id, _clock + delay);
  }

  return true;
}

}  // namespace aeronet::internal
