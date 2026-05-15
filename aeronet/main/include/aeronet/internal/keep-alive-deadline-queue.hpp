#pragma once

#include <chrono>
#include <cstdint>

#include "aeronet/connection-state.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::internal {

/// Min-heap priority queue that tracks each connection's keep-alive expiry deadline.
/// Supports O(log n) insertion, update, and arbitrary removal via per-entry index tracking stored in ConnectionState.
class KeepAliveDeadlineQueue {
 public:
  /// A single heap entry associating an expiry time with a connection state and its file descriptor.
  struct Entry {
    std::chrono::steady_clock::time_point expiresAt;
    ConnectionState* pState{nullptr};
    NativeHandle fd{kInvalidHandle};
  };

  /// Inserts a new deadline entry for state, or updates the existing one if already present.
  void upsert(ConnectionState& state, NativeHandle fd, std::chrono::steady_clock::time_point expiresAt);

  /// Removes the deadline entry associated with state; no-op if none is registered.
  void remove(ConnectionState& state);

  /// Removes and returns the entry with the earliest deadline (the heap minimum).
  Entry pop();

  /// Removes all entries and resets every associated ConnectionState index.
  void clear();

  /// Returns the entry with the earliest deadline without removing it.
  [[nodiscard]] const Entry& top() const noexcept { return _heap.front(); }

  /// Returns true if the queue contains no entries.
  [[nodiscard]] bool empty() const noexcept { return _heap.empty(); }

 private:
  void swapEntries(std::uint32_t lhsIdx, std::uint32_t rhsIdx) noexcept;
  void siftUp(std::uint32_t idx) noexcept;
  void siftDown(std::uint32_t idx) noexcept;
  void restoreHeapAt(std::uint32_t idx) noexcept;

  vector<Entry> _heap;
};

}  // namespace aeronet::internal
