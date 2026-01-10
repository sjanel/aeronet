#include "aeronet/decompression-config.hpp"

#include <stdexcept>

namespace aeronet {

void DecompressionConfig::validate() const {
  if (!enable) {
    return;
  }
#if !defined(AERONET_ENABLE_ZLIB) && !defined(AERONET_ENABLE_BROTLI) && !defined(AERONET_ENABLE_ZSTD)
  throw std::invalid_argument("Cannot enable automatic decompression when no decoder is compiled in");
#endif
  if (decoderChunkSize == 0) {
    throw std::invalid_argument("decoderChunkSize must be > 0");
  }
  if (maxDecompressedBytes < decoderChunkSize) {
    throw std::invalid_argument("maxDecompressedBytes must be >= decoderChunkSize");
  }
  if (maxExpansionRatio < 0.0) {
    throw std::invalid_argument("maxExpansionRatio must be >= 0");
  }
  if (maxCompressedBytes != 0 && maxCompressedBytes > (128UL * 1024UL * 1024UL * 1024UL)) {
    // Cap insane compressed size to catch likely misconfiguration
    throw std::invalid_argument("maxCompressedBytes is unreasonably large");
  }
}

}  // namespace aeronet
