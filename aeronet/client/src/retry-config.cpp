#include "aeronet/retry-config.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "aeronet/chrono-lite.hpp"
#include "aeronet/http-status-code.hpp"

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

RetryConfig::Duration RetryConfig::statusRetryDelay(std::string_view retryAfter, Duration computed) const noexcept {
  if (!honorRetryAfter) {
    return computed;
  }
  uint64_t seconds;
  const char* end = retryAfter.data() + retryAfter.size();
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  const auto [ptr, ec] = std::from_chars(retryAfter.data(), end, seconds);
  if (ec != std::errc{} || ptr != end) {
    return computed;  // absent, or not delta-seconds
  }
  const auto capSeconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(maxDelay).count());
  return std::chrono::duration_cast<Duration>(std::chrono::seconds{std::min(seconds, capSeconds)});
}

void RetryConfig::validate() const {
  if (baseDelay <= Duration{0}) {
    throw std::invalid_argument("baseDelay must be greater than 0");
  }
  if (maxDelay < baseDelay) {
    throw std::invalid_argument("maxDelay must be greater than or equal to baseDelay");
  }
  if (maxAttempts < 1) {
    throw std::invalid_argument("maxAttempts must be at least 1");
  }
  if (std::isnan(multiplier) || multiplier < 1.0F) {
    throw std::invalid_argument("multiplier must be greater than or equal to 1");
  }
  if (std::isnan(jitter) || jitter < 0.0F || jitter > 1.0F) {
    throw std::invalid_argument("jitter must be between 0 and 1");
  }
  if (std::ranges::any_of(retryStatuses,
                          [](http::StatusCode statusCode) { return statusCode < 400 || statusCode > 999; })) {
    throw std::invalid_argument("retryStatuses must contain only valid HTTP status codes >= 400");
  }
}

}  // namespace aeronet