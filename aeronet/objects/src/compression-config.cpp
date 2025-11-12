#include "aeronet/compression-config.hpp"

#include <stdexcept>

#include "aeronet/features.hpp"
#include "aeronet/log.hpp"

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>

#include "aeronet/zstd-encoder.hpp"
#endif

namespace aeronet {

void CompressionConfig::validate() const {
  if constexpr (aeronet::zlibEnabled()) {
    if (zlib.level != Zlib::kDefaultLevel && (zlib.level < Zlib::kMinLevel || zlib.level > Zlib::kMaxLevel)) {
      log::critical("Invalid ZLIB compression level {}", zlib.level);
      throw std::invalid_argument("Invalid ZLIB compression level");
    }
  }

#ifdef AERONET_ENABLE_ZSTD
  if (zstd.compressionLevel < ZSTD_minCLevel() || zstd.compressionLevel > ZSTD_maxCLevel()) {
    log::critical("Invalid ZSTD compression level {}", zstd.compressionLevel);
    throw std::invalid_argument("Invalid ZSTD compression level");
  }
  details::ZstdContextRAII testConstruction(zstd.compressionLevel, zstd.windowLog);
#endif

  if constexpr (aeronet::brotliEnabled()) {
    if (brotli.quality < Brotli::kMinQuality || brotli.quality > Brotli::kMaxQuality) {
      log::critical("Invalid Brotli quality {}", brotli.quality);
      throw std::invalid_argument("Invalid Brotli quality");
    }
    if (brotli.window < Brotli::kMinWindow || brotli.window > Brotli::kMaxWindow) {
      log::critical("Invalid Brotli window {}", brotli.window);
      throw std::invalid_argument("Invalid Brotli window");
    }
  }
}

}  // namespace aeronet