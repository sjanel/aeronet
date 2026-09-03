#include "aeronet/decompression-config.hpp"

#include <stdexcept>

namespace aeronet {

void DecompressionConfig::validate() const {
  if (!enable) {
    return;
  }
#if !defined(AERONET_ENABLE_ZLIB) && !defined(AERONET_ENABLE_BROTLI) && !defined(AERONET_ENABLE_ZSTD)
  throw std::invalid_argument("Cannot enable automatic decompression when no decoder is compiled in");
#else
  if (maxCompressedBytes == 0) {
    throw std::invalid_argument("DecompressionConfig: maxCompressedBytes must be > 0");
  }
  if (maxCompressedBytes > (128ULL << 30U)) {
    // Cap insane compressed size to catch likely misconfiguration
    throw std::invalid_argument("DecompressionConfig: maxCompressedBytes is unreasonably large");
  }
  if (decoderChunkSize == 0) {
    throw std::invalid_argument("DecompressionConfig: decoderChunkSize must be > 0");
  }
  if (maxDecompressedBytes < decoderChunkSize) {
    throw std::invalid_argument("DecompressionConfig: maxDecompressedBytes must be >= decoderChunkSize");
  }
  if (maxExpansionRatio <= 0.0) {
    throw std::invalid_argument("DecompressionConfig: maxExpansionRatio must be > 0");
  }
#endif
}

}  // namespace aeronet
