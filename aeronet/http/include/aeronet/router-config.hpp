#pragma once

#include <cstdint>

#include "aeronet/cors-policy.hpp"

namespace aeronet {

struct RouterConfig {
  enum class TrailingSlashPolicy : std::int8_t { Strict, Normalize, Redirect };

  // Behavior for resolving paths that differ only by a trailing slash.
  // Default: Normalize
  TrailingSlashPolicy trailingSlashPolicy{TrailingSlashPolicy::Normalize};

  // Optional default CORS policy applied when no per-route policy exists but a router-level default was configured
  CorsPolicy defaultCorsPolicy;

  // Policy for handling a trailing slash difference between registered path handlers and incoming requests.
  // Resolution algorithm (independent of policy):
  //   1. ALWAYS attempt an exact match on the incoming target string first. If found, dispatch that handler.
  //      (This means if both "/p" and "/p/" are registered, each is honored exactly as requested; no policy logic
  //      runs.)
  //   2. If no exact match:
  //        a) If the request ends with one trailing slash (not root) and the canonical form without the slash exists:
  //             - Strict   : treat as not found (404).
  //             - Normalize: internally treat it as the canonical path (strip slash, no redirect).
  //             - Redirect : emit a 301 with Location header pointing to the canonical (no trailing slash) path.
  //        b) Else if the request does NOT end with a slash, policy is Normalize, and ONLY the slashed variant exists
  //             (e.g. "/x/" registered, "/x" not): treat the slashed variant as equivalent and dispatch to it.
  //        c) Otherwise: 404 (no transformation / redirect performed).
  //   3. Root path "/" is never redirected or normalized.
  //
  // Summary:
  //   Strict   : exact-only matching; variants differ; no implicit mapping.
  //   Normalize: provide symmetric acceptance (one missing variant maps to the existing one) without redirects.
  //   Redirect : like Strict unless the ONLY difference is an added trailing slash for a canonical registered path;
  //              then a 301 to the canonical form is sent (never the inverse).
  RouterConfig& withTrailingSlashPolicy(TrailingSlashPolicy policy);

  // Sets a default CORS policy applied to all routes that do not have a per-route CORS policy configured.
  // If a route has its own CORS policy, that one takes precedence over the router-level default.
  RouterConfig& withDefaultCorsPolicy(CorsPolicy policy);
};

}  // namespace aeronet