#include "aeronet/internal/keep-alive-deadline-queue.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/native-handle.hpp"

namespace aeronet::internal {

namespace {

constexpr std::uint32_t ParentIndex(std::uint32_t idx) { return (idx - 1U) / 2U; }

constexpr std::uint32_t LeftChildIndex(std::uint32_t idx) { return (idx * 2U) + 1U; }

constexpr bool operator<(const KeepAliveDeadlineQueue::Entry& lhs, const KeepAliveDeadlineQueue::Entry& rhs) {
  if (lhs.expiresAt != rhs.expiresAt) {
    return lhs.expiresAt < rhs.expiresAt;
  }
  return lhs.fd < rhs.fd;
}

}  // namespace

void KeepAliveDeadlineQueue::swapEntries(std::uint32_t lhsIdx, std::uint32_t rhsIdx) noexcept {
  using std::swap;
  swap(_heap[lhsIdx], _heap[rhsIdx]);
  _heap[lhsIdx].state->keepAliveDeadlineIndex = lhsIdx;
  _heap[rhsIdx].state->keepAliveDeadlineIndex = rhsIdx;
}

void KeepAliveDeadlineQueue::siftUp(std::uint32_t idx) noexcept {
  while (idx > 0U) {
    const std::uint32_t parentIdx = ParentIndex(idx);
    if (_heap[idx] < _heap[parentIdx]) {
      swapEntries(idx, parentIdx);
      idx = parentIdx;
    } else {
      break;
    }
  }
}

void KeepAliveDeadlineQueue::siftDown(std::uint32_t idx) noexcept {
  const auto heapSize = static_cast<std::uint32_t>(_heap.size());
  for (;;) {
    const std::uint32_t leftIdx = LeftChildIndex(idx);
    if (leftIdx >= heapSize) {
      break;
    }
    const std::uint32_t rightIdx = leftIdx + 1U;
    std::uint32_t smallestIdx = leftIdx;
    if (rightIdx < heapSize && _heap[rightIdx] < _heap[leftIdx]) {
      smallestIdx = rightIdx;
    }
    if (_heap[smallestIdx] < _heap[idx]) {
      swapEntries(idx, smallestIdx);
      idx = smallestIdx;
    } else {
      break;
    }
  }
}

void KeepAliveDeadlineQueue::restoreHeapAt(std::uint32_t idx) noexcept {
  if (idx > 0U && _heap[idx] < _heap[ParentIndex(idx)]) {
    siftUp(idx);
    return;
  }
  siftDown(idx);
}

void KeepAliveDeadlineQueue::upsert(ConnectionState& state, NativeHandle fd,
                                    std::chrono::steady_clock::time_point expiresAt) {
  if (state.keepAliveDeadlineIndex == ConnectionState::kNoKeepAliveDeadlineIndex) {
    const auto idx = static_cast<std::uint32_t>(_heap.size());
    state.keepAliveDeadlineIndex = idx;
    _heap.emplace_back(expiresAt, &state, fd);
    siftUp(idx);
  } else {
    const auto idx = state.keepAliveDeadlineIndex;
    assert(_heap[idx].state == &state);
    _heap[idx].expiresAt = expiresAt;
    _heap[idx].fd = fd;
    restoreHeapAt(idx);
  }
}

void KeepAliveDeadlineQueue::remove(ConnectionState& state) {
  if (state.keepAliveDeadlineIndex == ConnectionState::kNoKeepAliveDeadlineIndex) {
    return;
  }

  const auto idx = state.keepAliveDeadlineIndex;
  assert(_heap[idx].state == &state);

  const auto lastIdx = static_cast<std::uint32_t>(_heap.size() - 1U);
  if (idx != lastIdx) {
    swapEntries(idx, lastIdx);
  }
  _heap.back().state->keepAliveDeadlineIndex = ConnectionState::kNoKeepAliveDeadlineIndex;
  _heap.pop_back();

  if (idx < _heap.size()) {
    restoreHeapAt(idx);
  }
}

KeepAliveDeadlineQueue::Entry KeepAliveDeadlineQueue::pop() {
  Entry entry = _heap.front();
  remove(*entry.state);
  return entry;
}

void KeepAliveDeadlineQueue::clear() {
  for (Entry& entry : _heap) {
    entry.state->keepAliveDeadlineIndex = ConnectionState::kNoKeepAliveDeadlineIndex;
  }
  _heap.clear();
}

}  // namespace aeronet::internal
