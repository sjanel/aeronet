#include "aeronet/safe-cast.hpp"

#include <cstdint>
#include <stdexcept>

#include "aeronet/log.hpp"

namespace aeronet {

void ThrowSafeCastNegative(std::intmax_t value) {
  log::critical("SafeCast: negative value {} cannot be represented in an unsigned target type", value);
  throw std::overflow_error("negative value cannot be represented in unsigned target type");
}

void ThrowSafeCastOverflow(std::intmax_t value, std::uintmax_t maxValue) {
  log::critical("SafeCast: value {} exceeds the target type maximum {}", value, maxValue);
  throw std::overflow_error("value exceeds target type maximum");
}

void ThrowSafeCastOverflow(std::uintmax_t value, std::uintmax_t maxValue) {
  log::critical("SafeCast: value {} exceeds the target type maximum {}", value, maxValue);
  throw std::overflow_error("value exceeds target type maximum");
}

}  // namespace aeronet
