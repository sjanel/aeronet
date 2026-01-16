#include "aeronet/compression-config.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

#include "aeronet/encoding.hpp"
#include "aeronet/features.hpp"

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>
#endif

namespace aeronet {

void CompressionConfig::validate() const {
  if (!std::isfinite(maxCompressRatio)) {
    throw std::invalid_argument("Invalid maxCompressRatio (expected finite value)");
  }
  if (maxCompressRatio <= 0.0 || maxCompressRatio >= 1.0) {
    throw std::invalid_argument(std::format("Invalid maxCompressRatio {} (expected 0 < ratio < 1)", maxCompressRatio));
  }
  if (minBytes < 16U) {
    throw std::invalid_argument("minBytes must be at least 16");
  }
  if constexpr (zlibEnabled()) {
    if (zlib.level != Zlib::kDefaultLevel && (zlib.level < Zlib::kMinLevel || zlib.level > Zlib::kMaxLevel)) {
      throw std::invalid_argument(std::format("Invalid ZLIB compression level {}", zlib.level));
    }
  }
  auto it = std::ranges::find_if_not(preferredFormats, [](Encoding enc) { return IsEncodingEnabled(enc); });
  if (it != preferredFormats.end()) {
    throw std::invalid_argument("Unsupported encoding in preferredFormats");
  }

  // check if preferredFormats has duplicates (forbidden)
  if (std::ranges::any_of(preferredFormats,
                          [this](Encoding enc) { return std::ranges::count(preferredFormats, enc) > 1; })) {
    throw std::invalid_argument("preferredFormats contains duplicate encodings");
  }

#ifdef AERONET_ENABLE_ZSTD
  if (zstd.compressionLevel < ZSTD_minCLevel() || zstd.compressionLevel > ZSTD_maxCLevel()) {
    throw std::invalid_argument(std::format("Invalid ZSTD compression level {}", zstd.compressionLevel));
  }
#endif

  if constexpr (brotliEnabled()) {
    if (brotli.quality < Brotli::kMinQuality || brotli.quality > Brotli::kMaxQuality) {
      throw std::invalid_argument(std::format("Invalid Brotli quality {}", brotli.quality));
    }
    if (brotli.window < Brotli::kMinWindow || brotli.window > Brotli::kMaxWindow) {
      throw std::invalid_argument(std::format("Invalid Brotli window {}", brotli.window));
    }
  }
}

}  // namespace aeronet