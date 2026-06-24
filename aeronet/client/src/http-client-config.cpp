#include "aeronet/http-client-config.hpp"

#include <limits>
#include <stdexcept>

namespace aeronet {

void HttpClientConfig::validate() const {
  const auto connectMs = connectTimeout.count();
  if (connectMs < 1 || connectMs > std::numeric_limits<int>::max()) {
    throw std::invalid_argument("connectTimeout must be between 1 ms and INT_MAX ms");
  }
}

}  // namespace aeronet