#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace aeronet::test {

/// Configuration for deterministic fault injection in TestTransport.
/// All fields default to "no faults" (unlimited bytes, no errors).
struct FaultPolicy {
  /// Maximum bytes returned per read() call (simulates partial delivery).
  /// 0 means "no limit" (return all available).
  std::size_t maxBytesPerRead{0};

  /// Maximum bytes accepted per write() call (simulates partial writes).
  /// 0 means "no limit" (accept all data).
  std::size_t maxBytesPerWrite{0};

  /// Return ReadReady (EAGAIN) every N read calls. 0 disables.
  uint32_t eagainAfterEveryNReads{0};

  /// Return WriteReady (EAGAIN) every N write calls. 0 disables.
  uint32_t eagainAfterEveryNWrites{0};

  /// Inject Error after this many total bytes have been read. max disables.
  std::size_t resetAfterTotalBytesRead{std::numeric_limits<std::size_t>::max()};

  /// Inject Error after this many total bytes have been written. max disables.
  std::size_t resetAfterTotalBytesWritten{std::numeric_limits<std::size_t>::max()};

  /// If true, next read() returns Error immediately (one-shot, auto-clears).
  bool resetOnNextRead{false};

  /// If true, next write() returns Error immediately (one-shot, auto-clears).
  bool resetOnNextWrite{false};

  /// Seed for deterministic PRNG (used when randomized partial sizes are desired).
  /// If maxBytesPerRead > 0, actual bytes returned per read is uniform in [1, maxBytesPerRead].
  /// If seed is 0 and maxBytesPerRead > 0, always returns exactly maxBytesPerRead (no randomization).
  uint64_t seed{0};
};

}  // namespace aeronet::test
