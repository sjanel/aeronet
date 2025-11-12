#include "aeronet/otel-config.hpp"

#include <stdexcept>

#include "aeronet/log.hpp"

namespace aeronet {

void OtelConfig::validate() const {
  if (sampleRate < 0.0 || sampleRate > 1.0) {
    log::critical("Invalid sample rate {}, must be between 0 and 1", sampleRate);
    throw std::invalid_argument("Invalid sample rate");
  }
}

}  // namespace aeronet