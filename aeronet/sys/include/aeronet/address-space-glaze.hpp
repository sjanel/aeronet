#pragma once

#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>

#include "aeronet/address-space.hpp"

template <>
struct glz::meta<aeronet::AddressSpace> {
  using enum aeronet::AddressSpace;
  static constexpr auto value = enumerate("loopback", Loopback, "local", Local, "public", Public);
};
