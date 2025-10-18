#include "aeronet/builtin-probes-config.hpp"

#include <algorithm>
#include <string_view>

#include "invalid_argument_exception.hpp"

namespace aeronet {

void BuiltinProbesConfig::validate() const {
  if (enabled) {
    auto checkPath = [](std::string_view path, std::string_view name) {
      if (path.empty()) {
        throw invalid_argument("builtin probe path '{}' must be non-empty", name);
      }
      if (path.front() != '/') {
        throw invalid_argument("builtin probe path '{}' must start with '/'", name);
      }
      // Disallow spaces and control characters in probe paths
      if (std::ranges::any_of(path, [](unsigned char ch) { return ch <= 0x1F || ch == 0x7F || ch == ' '; })) {
        throw invalid_argument("builtin probe path '{}' contains invalid characters", name);
      }
    };
    checkPath(livenessPath, "livenessPath");
    checkPath(readinessPath, "readinessPath");
    checkPath(startupPath, "startupPath");
  }
}

}  // namespace aeronet