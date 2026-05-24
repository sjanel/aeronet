#include "aeronet/access-log-config.hpp"

#include <stdexcept>

namespace aeronet {
void AccessLogConfig::validate() const {
  if (sink == Sink::File && filePath.empty()) {
    throw std::invalid_argument("Access log file path cannot be empty when sink is File");
  }
}
}  // namespace aeronet
