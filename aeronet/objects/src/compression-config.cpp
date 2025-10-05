#include "aeronet/compression-config.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include <zlib.h>

#include <limits>
#endif

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>

#include "zstd-encoder.hpp"
#endif

#ifdef AERONET_ENABLE_BROTLI
#include <brotli/encode.h>
#endif

#include "invalid_argument_exception.hpp"

namespace aeronet {

void CompressionConfig::validate() const {
#ifdef AERONET_ENABLE_ZLIB
  using Level = decltype(zlib.level);

  static_assert(Z_DEFAULT_COMPRESSION > std::numeric_limits<Level>::min());
  static_assert(Z_BEST_COMPRESSION < std::numeric_limits<Level>::max());

  if (zlib.level != Z_DEFAULT_COMPRESSION && (zlib.level < Z_BEST_SPEED || zlib.level > Z_BEST_COMPRESSION)) {
    throw invalid_argument("Invalid ZLIB compression level {}", zlib.level);
  }
#endif

#ifdef AERONET_ENABLE_ZSTD
  if (zstd.compressionLevel < ZSTD_minCLevel() || zstd.compressionLevel > ZSTD_maxCLevel()) {
    throw invalid_argument("Invalid ZSTD compression level {}", zstd.compressionLevel);
  }
  details::ZstdCStreamRAII testConstruction(zstd.compressionLevel, zstd.windowLog);
#endif

#ifdef AERONET_ENABLE_BROTLI
  if (brotli.quality < BROTLI_MIN_QUALITY || brotli.quality > BROTLI_MAX_QUALITY) {
    throw invalid_argument("Invalid Brotli quality {}", brotli.quality);
  }
  if (brotli.window < BROTLI_MIN_WINDOW_BITS || brotli.window > BROTLI_MAX_WINDOW_BITS) {
    throw invalid_argument("Invalid Brotli window {}", brotli.window);
  }
#endif
}

}  // namespace aeronet