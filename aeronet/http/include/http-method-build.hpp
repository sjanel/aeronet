// Helper functions for building and querying HTTP method bitmasks.
#pragma once

#include <cstdint>
#include <span>

#include "http-method.hpp"

namespace aeronet::http {

// Build a bitmask from a MethodSet (bit position == enum ordinal order).
constexpr uint32_t methodListToMask(std::span<const Method> methods) {
  uint32_t mask = 0;
  for (auto methodVal : methods) {
    mask |= (1U << static_cast<uint8_t>(methodVal));
  }
  return mask;
}

// Single method to mask helper.
constexpr uint32_t singleMethodToMask(Method method) { return 1U << static_cast<uint8_t>(method); }

// Check if a method is allowed by mask.
constexpr bool methodAllowed(uint32_t mask, Method method) {
  return (mask & (1U << static_cast<uint8_t>(method))) != 0U;
}

}  // namespace aeronet::http