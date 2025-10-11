#include "aeronet/router-config.hpp"

namespace aeronet {

RouterConfig& RouterConfig::withTrailingSlashPolicy(TrailingSlashPolicy policy) {
  trailingSlashPolicy = policy;
  return *this;
}

}  // namespace aeronet