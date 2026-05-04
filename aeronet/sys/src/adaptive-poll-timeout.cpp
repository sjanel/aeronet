#include "aeronet/adaptive-poll-timeout.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

#include "aeronet/timedef.hpp"

namespace aeronet {

void PollTimeoutPolicy::validate() const {
  const auto baseMs = std::chrono::duration_cast<std::chrono::milliseconds>(baseTimeout).count();
  if (baseMs <= 0) {
    throw std::invalid_argument("PollTimeoutPolicy: baseTimeout must be > 0");
  }
  if (minFactor < 0.0F || minFactor > 1.0F || !std::isfinite(minFactor)) {
    throw std::invalid_argument("PollTimeoutPolicy: minTimeout must be finite and >= 0");
  }
  if (maxFactor < 1.0F || !std::isfinite(maxFactor)) {
    throw std::invalid_argument(
        "PollTimeoutPolicy: maxTimeout must be finite, >= 1 and greater or equal to minTimeout");
  }
  const double scaledMs = static_cast<double>(baseMs) * static_cast<double>(maxFactor);
  if (scaledMs > static_cast<double>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("PollTimeoutPolicy: maxTimeout factor is too large and would cause overflow");
  }
}

namespace {

constexpr std::chrono::milliseconds ScalePollInterval(SysDuration baseTimeout, float factor) {
  const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(baseTimeout);
  const double scaledMs = static_cast<double>(interval.count()) * static_cast<double>(factor);
  return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(scaledMs)};
}

constexpr int DurationToPollTimeoutMs(SysDuration timeout) {
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
  assert(milliseconds >= 0);
  assert(!std::cmp_greater(milliseconds, std::numeric_limits<int>::max()));
  return static_cast<int>(milliseconds);
}

constexpr int MultiplyTimeoutWithCap(int timeoutMs, uint32_t growthFactor, int maxTimeoutMs) {
  const int floorMs = std::max(timeoutMs, 1);
  const auto scaledTimeoutMs = static_cast<std::uint64_t>(floorMs) * static_cast<std::uint64_t>(growthFactor);
  if (std::cmp_less(scaledTimeoutMs, maxTimeoutMs)) {
    return static_cast<int>(scaledTimeoutMs);
  }
  return maxTimeoutMs;
}

}  // namespace

AdaptivePollTimeout::AdaptivePollTimeout(PollTimeoutPolicy policy)
    : _minPollTimeoutMs(DurationToPollTimeoutMs(ScalePollInterval(policy.baseTimeout, policy.minFactor))),
      _maxPollTimeoutMs(DurationToPollTimeoutMs(ScalePollInterval(policy.baseTimeout, policy.maxFactor))),
      _basePollTimeoutMs(DurationToPollTimeoutMs(policy.baseTimeout)),
      _pollTimeoutMs(_basePollTimeoutMs) {
  assert(_maxPollTimeoutMs >= _minPollTimeoutMs);
  assert(_basePollTimeoutMs >= _minPollTimeoutMs && _basePollTimeoutMs <= _maxPollTimeoutMs);
}

void AdaptivePollTimeout::update(uint32_t nbReadyEvents, uint32_t capacityBeforePoll) {
  // Saturation: every event slot was filled. Drop to the minimum (often 0) so the next poll
  // returns immediately and we drain whatever else is waiting in the kernel queue.
  assert(capacityBeforePoll > 0U);
  if (nbReadyEvents == capacityBeforePoll) {
    _pollTimeoutMs = _minPollTimeoutMs;
    _idlePollIterations = 0;
    return;
  }

  // Idle: no events at all. Increase the timeout exponentially after enough consecutive idle
  // polls, capped at maxTimeout. Note: when transitioning saturation -> idle the backoff grows
  // from the current (small) timeout rather than restarting at base; this is intentional and
  // gives a gentle decay back to the maximum sleep when the server truly goes quiet.
  if (nbReadyEvents == 0U) {
    static constexpr uint32_t kDefaultIdleIterationsBeforeBackoff = 4;
    static constexpr uint32_t kDefaultPollTimeoutGrowthFactor = 2;

    ++_idlePollIterations;
    if (_idlePollIterations >= kDefaultIdleIterationsBeforeBackoff) {
      _pollTimeoutMs = MultiplyTimeoutWithCap(_pollTimeoutMs, kDefaultPollTimeoutGrowthFactor, _maxPollTimeoutMs);
      _idlePollIterations = 0;
    }
    return;
  }

  // Normal load: some but not all slots used. Reset to the configured base so we neither spin
  // nor stay in backoff mode.
  _pollTimeoutMs = _basePollTimeoutMs;
  _idlePollIterations = 0;
}

}  // namespace aeronet