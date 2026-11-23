#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace aeronet::tracing::detail {

// Decides which spans are recorded, at a rate in [0, 1], before any span is started: an unsampled span costs no
// allocation nor OpenTelemetry SDK call. aeronet starts its spans without parent context, each in a new trace with a
// random id: a random draw per span samples like the trace id ratio sampler of the SDK, which compares the random part
// of the trace id with the same threshold.
// Not thread-safe: each TelemetryContext, owned by a single server or client, has its own.
class SpanSampler {
 public:
  SpanSampler(float rate, uint64_t seed) noexcept : _threshold(Threshold(rate)), _state(seed) {}

  // Tells whether the next span is recorded.
  [[nodiscard]] bool sample() noexcept {
    if (_threshold == kSampleAll) {
      return true;
    }
    _state += kGamma;
    return Mix(_state) < _threshold;
  }

 private:
  static constexpr uint64_t kSampleAll = std::numeric_limits<uint64_t>::max();
  static constexpr uint64_t kGamma = 0x9E3779B97F4A7C15ULL;

  // A draw is sampled when below the threshold: rate * 2^64, all of them from 1, none from 0 (or NaN).
  static uint64_t Threshold(float rate) noexcept {
    if (rate >= 1.0F) {
      return kSampleAll;
    }
    if (!(rate > 0.0F)) {
      return 0;
    }
    // Below 2^64: the largest float below 1 is 1 - 2^-24.
    return static_cast<uint64_t>(std::ldexp(static_cast<double>(rate), std::numeric_limits<uint64_t>::digits));
  }

  // splitmix64 finalizer: a well-mixed 64-bit value per step of the sequence.
  static constexpr uint64_t Mix(uint64_t value) noexcept {
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
  }

  uint64_t _threshold;
  uint64_t _state;
};

}  // namespace aeronet::tracing::detail
