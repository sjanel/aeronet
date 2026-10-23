#pragma once

#include <glaze/glaze.hpp>
#include <type_traits>

#include "aeronet/access-log-config-glaze.hpp"          // IWYU pragma: export
#include "aeronet/builtin-probes-config-glaze.hpp"      // IWYU pragma: export
#include "aeronet/direct-compression-mode-glaze.hpp"    // IWYU pragma: export
#include "aeronet/encoding-glaze.hpp"                   // IWYU pragma: export
#include "aeronet/glaze-chrono-durations-adapters.hpp"  // IWYU pragma: export
#include "aeronet/http-server-config.hpp"
#include "aeronet/tcp-no-delay-mode-glaze.hpp"  // IWYU pragma: export
#include "aeronet/telemetry-config-glaze.hpp"   // IWYU pragma: export
#include "aeronet/tls-config-glaze.hpp"         // IWYU pragma: export
#include "aeronet/zerocopy-mode-glaze.hpp"      // IWYU pragma: export

// No glz::meta<HttpServerConfig>: glaze reflects all its members, so that a new field is serialized automatically.
static_assert(std::is_aggregate_v<aeronet::HttpServerConfig>,
              "HttpServerConfig must stay an aggregate (no private fields, no constructors) to be reflected by glaze");

template <>
struct glz::meta<aeronet::HttpServerConfig::TraceMethodPolicy> {
  using enum aeronet::HttpServerConfig::TraceMethodPolicy;
  static constexpr auto value =
      enumerate("disabled", Disabled, "enabledPlainAndTLS", EnabledPlainAndTLS, "enabledPlainOnly", EnabledPlainOnly);
};
