#pragma once

#include <cstdint>

namespace aeronet::tracing::detail {

// SplitMix64 (Steele, Lea, Flood 2014): a 64-bit state advanced by a constant odd step, each value its well-mixed
// finalization. Fast, one value per call, every 64-bit value once over a period of 2^64. Not cryptographic.
class SplitMix64 {
 public:
  explicit SplitMix64(uint64_t seed) noexcept : _state(seed) {}

  [[nodiscard]] uint64_t next() noexcept {
    _state += 0x9E3779B97F4A7C15ULL;
    uint64_t value = _state;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
  }

 private:
  uint64_t _state;
};

}  // namespace aeronet::tracing::detail
