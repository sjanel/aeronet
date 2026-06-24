#include "aeronet/retry-config.hpp"

#include <algorithm>
#include <cstdint>

namespace aeronet {

// Backoff delay before the retry numbered `retryIndex` (0-based: 0 == the delay before the 1st retry).
// Exponential: baseDelay * multiplier^retryIndex, capped at maxDelay, then (when `jitter` > 0) scaled by
// a factor in [1 - jitter, 1 + jitter] driven by `rnd01` in [0, 1) -- `rnd01` is ignored when jitter==0,
// keeping the result deterministic. The final value is clamped to [0, maxDelay].
RetryConfig::Duration RetryConfig::delayFor(uint32_t retryIndex, double rnd01) const noexcept {
  const auto capMs = static_cast<double>(maxDelay.count());
  double delayMs = static_cast<double>(baseDelay.count());
  for (uint32_t step = 0; step < retryIndex && delayMs < capMs; ++step) {
    delayMs *= static_cast<double>(multiplier);
  }
  delayMs = std::min(delayMs, capMs);
  if (jitter > 0.0F) {
    const double factor = 1.0 + (static_cast<double>(jitter) * ((2.0 * rnd01) - 1.0));
    delayMs = std::clamp(delayMs * factor, 0.0, capMs);
  }
  return Duration{static_cast<Duration::rep>(delayMs)};
}

}  // namespace aeronet