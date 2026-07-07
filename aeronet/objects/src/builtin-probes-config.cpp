#include "aeronet/builtin-probes-config.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string_view>

#include "aeronet/log.hpp"

namespace aeronet {

void BuiltinProbesConfig::validate() const {
  if (enabled) {
    auto checkPath = [](std::string_view path, std::string_view name) {
      if (path.empty()) {
        log::critical("builtin probe path '{}' must be non-empty", name);
        throw std::invalid_argument("builtin probe path must be non-empty");
      }
      if (path.front() != '/') {
        log::critical("builtin probe path '{}' must start with '/'", name);
        throw std::invalid_argument("builtin probe path must start with '/'");
      }
      // Disallow spaces and control characters in probe paths
      if (std::ranges::any_of(path, [](unsigned char ch) { return ch <= 0x1F || ch == 0x7F || ch == ' '; })) {
        log::critical("builtin probe path '{}' contains invalid characters", name);
        throw std::invalid_argument("builtin probe path contains invalid characters");
      }
    };
    checkPath(livenessPath(), "livenessPath");
    checkPath(readinessPath(), "readinessPath");
    checkPath(startupPath(), "startupPath");

    if (dedicatedPort != 0 && livenessStaleThreshold <= std::chrono::milliseconds::zero()) {
      log::critical("builtin probe livenessStaleThreshold must be strictly positive when a dedicated port is set");
      throw std::invalid_argument("builtin probe livenessStaleThreshold must be strictly positive");
    }
  }
}

}  // namespace aeronet