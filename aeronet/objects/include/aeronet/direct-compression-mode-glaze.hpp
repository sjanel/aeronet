#pragma once

#include <glaze/glaze.hpp>

#include "aeronet/direct-compression-mode.hpp"

template <>
struct glz::meta<aeronet::DirectCompressionMode> {
  using enum aeronet::DirectCompressionMode;
  static constexpr auto value = enumerate("auto", Auto, "off", Off, "on", On);
};
