#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

#include "split-mix64.hpp"

namespace aeronet::tracing::detail {

// Decides which spans are recorded, at a rate in [0, 1], before any span is started: an unsampled span costs no
// record nor encoding. aeronet starts its spans without parent context, each in a new trace with a
// random id: a random draw per span samples like the trace id ratio sampler of the SDK, which compares the random part
// of the trace id with the same threshold.
// Not thread-safe: each TelemetryContext, owned by a single server or client, has its own.
class SpanSampler {
 public:
  SpanSampler(float rate, uint64_t seed) noexcept : _threshold(Threshold(rate)), _random(seed) {}

  // Tells whether the next span is recorded.
  [[nodiscard]] bool sample() noexcept {
    if (_threshold == kSampleAll) {
      return true;
    }
    return _random.next() < _threshold;
  }

 private:
  static constexpr uint64_t kSampleAll = std::numeric_limits<uint64_t>::max();

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

  uint64_t _threshold;
  SplitMix64 _random;
};

}  // namespace aeronet::tracing::detail
