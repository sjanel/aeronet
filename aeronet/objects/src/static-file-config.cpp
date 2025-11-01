#include "aeronet/static-file-config.hpp"

#include "invalid_argument_exception.hpp"

namespace aeronet {

void StaticFileConfig::validate() const {
  if (defaultIndex().contains('/') || defaultIndex().contains('\\')) {
    throw invalid_argument("StaticFileConfig.defaultIndex must not contain path separators");
  }
}

}  // namespace aeronet