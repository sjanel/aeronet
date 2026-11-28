#pragma once

#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>

#include "aeronet/tcp-no-delay-mode.hpp"

template <>
struct glz::meta<aeronet::TcpNoDelayMode> {
  using enum aeronet::TcpNoDelayMode;
  static constexpr auto value = enumerate("auto", Auto, "disabled", Disabled, "enabled", Enabled);
};
