#pragma once

#include <glaze/glaze.hpp>

#include "aeronet/tcp-no-delay-mode.hpp"

template <>
struct glz::meta<aeronet::TcpNoDelayMode> {
  using enum aeronet::TcpNoDelayMode;
  static constexpr auto value = enumerate("auto", Auto, "disabled", Disabled, "enabled", Enabled);
};
