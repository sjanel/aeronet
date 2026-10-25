#include "aeronet/static-concatenated-strings.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "aeronet/log.hpp"

namespace aeronet {

void ThrowStaticConcatenatedStringsPartOverflow(std::size_t partSize, std::uintmax_t maxSize) {
  log::critical("StaticConcatenatedStrings: part size {} must be lower than {}", partSize, maxSize);
  throw std::overflow_error("StaticConcatenatedStrings: part size exceeds maximum");
}

void ThrowStaticConcatenatedStringsTotalOverflow(std::uintmax_t totalSize, std::uintmax_t maxSize) {
  log::critical("StaticConcatenatedStrings: total size {} must be lower than {}", totalSize, maxSize);
  throw std::overflow_error("StaticConcatenatedStrings: total size exceeds maximum");
}

void ThrowStaticConcatenatedStringsDifferentNumberOfParts(std::size_t nbParts, std::size_t expectedNbParts) {
  log::critical("StaticConcatenatedStrings: {} parts provided instead of {}", nbParts, expectedNbParts);
  throw std::length_error("StaticConcatenatedStrings: must provide exactly the compile-time number of parts");
}

void ThrowStaticConcatenatedStringsTooManyParts(std::size_t maxNbParts) {
  log::critical("StaticConcatenatedStrings::emplace: all {} parts are already set", maxNbParts);
  throw std::length_error("StaticConcatenatedStrings::emplace: too many parts");
}

}  // namespace aeronet
