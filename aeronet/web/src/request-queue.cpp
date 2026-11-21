#include "request-queue.hpp"

#include <quickjs.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include "memory-budget.hpp"

namespace aeronet::internal {

RequestQueue::~RequestQueue() {
  for (const Pending& pending : _pending) {
    JS_FreeValue(_pCtx, pending.request.callback);
  }
}

int32_t RequestQueue::add(Request request, JSValueConst callback) {
  if (!_pMemory->canHold(request.memory)) {
    return 0;
  }
  // The ids only identify the pending requests: few at a time, they stay unique after a wrap around.
  const int32_t id = _nextId;
  _nextId = _nextId == std::numeric_limits<int32_t>::max() ? 1 : _nextId + 1;
  const std::size_t memory = request.memory;
  Pending& pending = _pending.emplace_back(std::move(request), id);  // the only step that can throw
  pending.request.callback = JS_DupValue(_pCtx, callback);
  _pMemory->charge(memory);
  return id;
}

void RequestQueue::cancel(int32_t id) noexcept {
  const auto pendingIt = std::ranges::find(_pending, id, &Pending::id);
  assert(pendingIt != _pending.end());
  _pMemory->release(pendingIt->request.memory);
  JS_FreeValue(_pCtx, pendingIt->request.callback);
  _pending.erase(pendingIt);
}

RequestQueue::Request RequestQueue::takeNext() noexcept {
  assert(!_pending.empty());
  Request request = std::move(_pending.front().request);
  _pending.erase(_pending.begin());
  _pMemory->release(request.memory);
  return request;
}

}  // namespace aeronet::internal
