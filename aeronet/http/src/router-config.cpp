#include "aeronet/router-config.hpp"

#include <stdexcept>
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

void RouterConfig::validate() const {
  auto raw = static_cast<std::underlying_type_t<TrailingSlashPolicy>>(trailingSlashPolicy);
  if (raw < 0 || raw > static_cast<std::underlying_type_t<TrailingSlashPolicy>>(TrailingSlashPolicy::Redirect)) {
    throw std::invalid_argument("Invalid TrailingSlashPolicy value");
  }
}

}  // namespace aeronet