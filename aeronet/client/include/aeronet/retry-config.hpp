#pragma once

#include <amc/allocator.hpp>
#include <chrono>
#include <cstdint>
#include <functional>

#include "aeronet/flat-set.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/small-vector.hpp"

namespace aeronet {

// Transparent retry + exponential-backoff policy for HttpClient. Subsumes the previous single-knob `maxRetries`. Two
// cooperating retry layers exist:
//
//   * The always-on, *free* pre-send stale-pool retry (independent of this config): a pooled keep-alive connection the
//   origin closed under us -- the request never reached the wire -- is silently re-issued on a fresh connection. It is
//   naturally bounded (it can only consume an origin's pooled connections), spends no `maxAttempts` budget and never
//   sleeps, because nothing was transmitted so it is always safe. This is exactly the behaviour the default config
//   (`maxAttempts == 1`) preserves.
//
//   * The configurable backoff retry governed by this struct: a connect failure, a *post-send* failure on an idempotent
//   method (only when `retryIdempotentAfterSend`), or a response whose status is in `retryStatuses` is retried after an
//   exponential-backoff sleep, up to `maxAttempts` total tries.
//
// Backoff is a blocking sleep: acceptable because HttpClient is already synchronous and blocking.
struct RetryConfig {
  using Duration = std::chrono::milliseconds;
  using RetryStatuses =
      FlatSet<http::StatusCode, std::less<>, amc::allocator<http::StatusCode>, SmallVector<http::StatusCode, 4U>>;

  // Backoff delay before the retry numbered `retryIndex` (0-based: 0 == the delay before the 1st retry).
  // Exponential: baseDelay * multiplier^retryIndex, capped at maxDelay, then (when `jitter` > 0) scaled by
  // a factor in [1 - jitter, 1 + jitter] driven by `rnd01` in [0, 1) -- `rnd01` is ignored when jitter==0,
  // keeping the result deterministic. The final value is clamped to [0, maxDelay].
  [[nodiscard]] Duration delayFor(uint32_t retryIndex, double rnd01 = 0.0) const noexcept;

  void validate() const;

  // The starting delay before the first retry.
  Duration baseDelay{std::chrono::milliseconds{100}};

  // Maximum backoff delay after adjustment for subsequent retries.
  Duration maxDelay{std::chrono::seconds{2}};

  // Maximum total attempts for one exchange. 1 (the default) disables backoff retries;
  // the free pre-send stale-pool retry described above is independent and still applies.
  // Must be > 0.
  uint32_t maxAttempts{1};

  // Multiplier of the delay applied for each subsequent retry. The delay for retry `i` is computed as `baseDelay *
  // multiplier^i`, capped at `maxDelay` and then optionally jittered.
  float multiplier{2.0F};

  // Fractional jitter applied multiplicatively to the computed exponential-backoff delay, so a fleet of clients doesn't
  // retry in lockstep. Must be in [0.0, 1.0] (enforced by validate()):
  //   * 0.0 (default): no jitter - delayFor() is fully deterministic.
  //   * j in (0.0, 1.0]: each retry's delay is scaled by a factor drawn uniformly from  half-open, since the underlying
  //   RNG draws from [0, 1) - then clamped to [0, maxDelay]. At j == 1.0 ("full jitter"), the scaled delay ranges from
  //   0 up to (but not including) twice the unscaled value.
  // Negative values are rejected: jitter is a magnitude, and a negative one would just reproduce the same delay
  // distribution as an equivalent positive value (rnd01 is uniform), so it's a confusing second spelling rather than a
  // distinct behavior. Values above 1.0 are rejected too: they only push more of the factor range below 0, which the
  // clamp turns into delay == 0 -- i.e. more retries firing immediately with no backoff at all, which is the opposite
  // of what jitter is for.
  float jitter{0.0F};

  // Retry a *post-send* failure (a transport/response error after the request bytes were written), but only ever for an
  // idempotent method and only when this is set: a retried post-send request is a re-submission.
  bool retryIdempotentAfterSend{false};

  // Honour a delta-seconds `Retry-After` response header on a status retry (capped at maxDelay). An HTTP-date form is
  // not parsed and falls back to the computed backoff.
  bool honorRetryAfter{true};

  // Response statuses that trigger a retry when `maxAttempts > 1`. Empty disables status-based retries.
  // Must contain valid HTTP status codes larger than or equal to 400.
  RetryStatuses retryStatuses{http::StatusCodeTooManyRequests, http::StatusCodeServiceUnavailable};
};

}  // namespace aeronet
