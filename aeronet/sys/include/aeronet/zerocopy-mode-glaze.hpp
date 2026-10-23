#pragma once

#include <glaze/glaze.hpp>

#include "aeronet/zerocopy-mode.hpp"

template <>
struct glz::meta<aeronet::ZerocopyMode> {
  using enum aeronet::ZerocopyMode;
  static constexpr auto value = enumerate("disabled", Disabled, "opportunistic", Opportunistic, "enabled", Enabled);
};
