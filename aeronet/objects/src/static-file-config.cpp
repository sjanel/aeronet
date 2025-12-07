#include "aeronet/static-file-config.hpp"

#include <stdexcept>

namespace aeronet {

void StaticFileConfig::validate() const {
  if (defaultIndex().contains('/') || defaultIndex().contains('\\')) {
    throw std::invalid_argument("StaticFileConfig.defaultIndex must not contain path separators");
  }
  if (defaultContentType().empty()) {
    throw std::invalid_argument("StaticFileConfig.defaultContentType cannot be empty");
  }
}

}  // namespace aeronet