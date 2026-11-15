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
  // These bodies are built in memory: larger, they would exceed the output limits of a connection (answered 503 over
  // HTTP/2, closing the HTTP/1.1 connection after them).
  if (maxMultipartBodySize > kMaxInMemoryBodyBytes) {
    throw std::invalid_argument("StaticFileConfig.maxMultipartBodySize cannot exceed kMaxInMemoryBodyBytes (2 MiB)");
  }
  if (inlineFileThresholdBytes > kMaxInMemoryBodyBytes) {
    throw std::invalid_argument(
        "StaticFileConfig.inlineFileThresholdBytes cannot exceed kMaxInMemoryBodyBytes (2 MiB)");
  }
}

}  // namespace aeronet