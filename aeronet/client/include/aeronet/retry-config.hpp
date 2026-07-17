#pragma once

#include <chrono>
#include <cstdint>

#include "aeronet/http-status-code.hpp"
#include "aeronet/small-vector.hpp"

namespace aeronet {

// Transparent retry + exponential-backoff policy for HttpClient. Subsumes the previous single-knob
// `maxRetries`. Two cooperating retry layers exist:
//
//   * The always-on, *free* pre-send stale-pool retry (independent of this config): a pooled keep-alive
//     connection the origin closed under us -- the request never reached the wire -- is silently re-issued
//     on a fresh connection. It is naturally bounded (it can only consume an origin's pooled connections),
//     spends no `maxAttempts` budget and never sleeps, because nothing was transmitted so it is always
//     safe. This is exactly the behaviour the default config (`maxAttempts == 1`) preserves.
//
//   * The configurable backoff retry governed by this struct: a connect failure, a *post-send* failure on
//     an idempotent method (only when `retryIdempotentAfterSend`), or a response whose status is in
//     `retryStatuses` is retried after an exponential-backoff sleep, up to `maxAttempts` total tries.
//
// Backoff is a blocking sleep: acceptable because HttpClient is already synchronous and blocking.
struct RetryConfig {
  using Duration = std::chrono::milliseconds;
  using RetryStatuses = SmallVector<http::StatusCode, 4>;

  // Backoff delay before the retry numbered `retryIndex` (0-based: 0 == the delay before the 1st retry).
  // Exponential: baseDelay * multiplier^retryIndex, capped at maxDelay, then (when `jitter` > 0) scaled by
  // a factor in [1 - jitter, 1 + jitter] driven by `rnd01` in [0, 1) -- `rnd01` is ignored when jitter==0,
  // keeping the result deterministic. The final value is clamped to [0, maxDelay].
  [[nodiscard]] Duration delayFor(uint32_t retryIndex, double rnd01 = 0.0) const noexcept;

  // Maximum total attempts for one exchange (a value < 1 is treated as 1). 1 (the default) disables backoff
  // retries; the free pre-send stale-pool retry described above is independent and still applies.
  Duration baseDelay{std::chrono::milliseconds{100}};
  Duration maxDelay{std::chrono::seconds{2}};
  uint32_t maxAttempts{1};
  float multiplier{2.0F};
  float jitter{0.0F};
  // Retry a *post-send* failure (a transport/response error after the request bytes were written), but only
  // ever for an idempotent method and only when this is set: a retried post-send request is a re-submission.
  bool retryIdempotentAfterSend{false};
  // Honour a delta-seconds `Retry-After` response header on a status retry (capped at maxDelay). An
  // HTTP-date form is not parsed and falls back to the computed backoff.
  bool honorRetryAfter{true};
  // Response statuses that trigger a retry when `maxAttempts > 1`. Empty disables status-based retries.
  RetryStatuses retryStatuses{http::StatusCodeTooManyRequests, http::StatusCodeServiceUnavailable};
};

}  // namespace aeronet
