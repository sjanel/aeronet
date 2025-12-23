#include "aeronet/router-config.hpp"

#include <utility>

#include "aeronet/cors-policy.hpp"

namespace aeronet {

RouterConfig& RouterConfig::withTrailingSlashPolicy(TrailingSlashPolicy policy) {
  trailingSlashPolicy = policy;
  return *this;
}

RouterConfig& RouterConfig::withDefaultCorsPolicy(CorsPolicy policy) {
  defaultCorsPolicy = std::move(policy);
  return *this;
}

}  // namespace aeronet