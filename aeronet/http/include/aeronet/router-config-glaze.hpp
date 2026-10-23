#pragma once

#include <glaze/glaze.hpp>

#include "aeronet/cors-policy-glaze.hpp"  // IWYU pragma: export
#include "aeronet/router-config.hpp"

template <>
struct glz::meta<aeronet::RouterConfig::TrailingSlashPolicy> {
  using enum aeronet::RouterConfig::TrailingSlashPolicy;
  static constexpr auto value = enumerate("strict", Strict, "normalize", Normalize, "redirect", Redirect);
};
