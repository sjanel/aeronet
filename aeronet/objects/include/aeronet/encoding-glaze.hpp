#pragma once

#include <glaze/glaze.hpp>

#include "aeronet/encoding.hpp"

template <>
struct glz::meta<aeronet::Encoding> {
  using enum aeronet::Encoding;
  static constexpr auto value = enumerate(zstd, br, gzip, deflate, none);
};
