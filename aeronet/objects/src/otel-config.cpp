#include "aeronet/otel-config.hpp"

#include "invalid_argument_exception.hpp"

namespace aeronet {

void OtelConfig::validate() const {
  if (sampleRate < 0.0 || sampleRate > 1.0) {
    throw invalid_argument("Invalid sample rate {}, must be between 0 and 1", sampleRate);
  }
}

}  // namespace aeronet