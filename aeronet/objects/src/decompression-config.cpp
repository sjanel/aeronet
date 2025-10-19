#include "aeronet/decompression-config.hpp"

#include "invalid_argument_exception.hpp"

namespace aeronet {

void DecompressionConfig::validate() const {
  if (decoderChunkSize == 0) {
    throw invalid_argument("decoderChunkSize must be > 0");
  }
  if (maxDecompressedBytes != 0 && maxDecompressedBytes < decoderChunkSize) {
    throw invalid_argument("maxDecompressedBytes must be >= decoderChunkSize");
  }
  if (maxExpansionRatio < 0.0) {
    throw invalid_argument("maxExpansionRatio must be >= 0");
  }
  if (maxCompressedBytes != 0 && maxCompressedBytes > (128UL * 1024UL * 1024UL * 1024UL)) {
    // Cap insane compressed size to catch likely misconfiguration
    throw invalid_argument("maxCompressedBytes is unreasonably large");
  }
}

}  // namespace aeronet
