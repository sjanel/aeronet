#pragma once

#include <chrono>
#include <cstdint>

#include "aeronet/connection-state.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::internal {

class KeepAliveDeadlineQueue {
 public:
  struct Entry {
    std::chrono::steady_clock::time_point expiresAt;
    ConnectionState* state{nullptr};
    NativeHandle fd{kInvalidHandle};
  };

  void upsert(ConnectionState& state, NativeHandle fd, std::chrono::steady_clock::time_point expiresAt);

  void remove(ConnectionState& state);

  Entry pop();

  void clear();

  [[nodiscard]] const Entry& top() const noexcept { return _heap.front(); }

  [[nodiscard]] bool empty() const noexcept { return _heap.empty(); }

 private:
  void swapEntries(std::uint32_t lhsIdx, std::uint32_t rhsIdx) noexcept;
  void siftUp(std::uint32_t idx) noexcept;
  void siftDown(std::uint32_t idx) noexcept;
  void restoreHeapAt(std::uint32_t idx) noexcept;

  vector<Entry> _heap;
};

}  // namespace aeronet::internal
