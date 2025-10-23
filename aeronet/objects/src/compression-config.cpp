#include "aeronet/compression-config.hpp"

#include "aeronet/features.hpp"

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>

#include "zstd-encoder.hpp"
#endif

#include "invalid_argument_exception.hpp"

namespace aeronet {

void CompressionConfig::validate() const {
  if constexpr (aeronet::zlibEnabled()) {
    if (zlib.level != Zlib::kDefaultLevel && (zlib.level < Zlib::kMinLevel || zlib.level > Zlib::kMaxLevel)) {
      throw invalid_argument("Invalid ZLIB compression level {}", zlib.level);
    }
  }

#ifdef AERONET_ENABLE_ZSTD
  if (zstd.compressionLevel < ZSTD_minCLevel() || zstd.compressionLevel > ZSTD_maxCLevel()) {
    throw invalid_argument("Invalid ZSTD compression level {}", zstd.compressionLevel);
  }
  details::ZstdCStreamRAII testConstruction(zstd.compressionLevel, zstd.windowLog);
#endif

  if constexpr (aeronet::brotliEnabled()) {
    if (brotli.quality < Brotli::kMinQuality || brotli.quality > Brotli::kMaxQuality) {
      throw invalid_argument("Invalid Brotli quality {}", brotli.quality);
    }
    if (brotli.window < Brotli::kMinWindow || brotli.window > Brotli::kMaxWindow) {
      throw invalid_argument("Invalid Brotli window {}", brotli.window);
    }
  }
}

}  // namespace aeronet