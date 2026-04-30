#pragma once

#include <cstdint>

#include "aeronet/timedef.hpp"

namespace aeronet {

struct PollTimeoutPolicy {
  // Throws std::invalid_argument if the policy is inconsistent:
  // baseTimeout must be > 0, minTimeout must be in [0, baseTimeout],
  // maxTimeout must be >= baseTimeout and fit within the poll API limit (INT_MAX ms).
  void validate() const;

  SysDuration baseTimeout{};
  float minFactor{1};
  float maxFactor{1};
};

// Encapsulates the adaptive poll-timeout state machine: holds the four boundary
// values and the idle-iteration counter, and exposes a single read accessor plus
// two mutating operations (reset to a new policy, update after each poll call).
class AdaptivePollTimeout {
 public:
  AdaptivePollTimeout() noexcept = default;

  // Creates a new AdaptivePollTimeout with the given policy. The policy must already be validated.
  explicit AdaptivePollTimeout(PollTimeoutPolicy policy);

  // Returns the current effective poll timeout in milliseconds.
  [[nodiscard]] int pollTimeoutMs() const { return _pollTimeoutMs; }

  // Adjusts the current timeout based on the last poll result.
  void update(uint32_t nbReadyEvents, uint32_t capacityBeforePoll);

 private:
  uint32_t _idlePollIterations = 0;
  int _minPollTimeoutMs = 0;
  int _maxPollTimeoutMs = 0;
  int _basePollTimeoutMs = 0;
  int _pollTimeoutMs = 0;
};

}  // namespace aeronet