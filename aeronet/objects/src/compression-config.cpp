#include "aeronet/compression-config.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <type_traits>

#include "aeronet/encoding.hpp"

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>

#include <cassert>
#endif

namespace aeronet {

void CompressionConfig::validate() const {
  if (!std::isfinite(maxCompressRatio)) {
    throw std::invalid_argument("Invalid maxCompressRatio (expected finite value)");
  }
  if (maxCompressRatio <= 0.0 || maxCompressRatio >= 1.0) {
    throw std::invalid_argument("Invalid maxCompressRatio, should be > 0.0 and < 1.0");
  }
  if (minBytes < 1U) {
    throw std::invalid_argument("minBytes must be at least 1");
  }

  if (std::ranges::any_of(preferredFormats, [](Encoding enc) {
        return static_cast<std::underlying_type_t<Encoding>>(enc) >= kNbContentEncodings;
      })) {
    throw std::invalid_argument("preferredFormats contains invalid encodings");
  }

#if !defined(AERONET_ENABLE_ZLIB) || !defined(AERONET_ENABLE_ZSTD) || !defined(AERONET_ENABLE_BROTLI)
  if (!std::ranges::all_of(preferredFormats, [](Encoding enc) { return IsEncodingEnabled(enc); })) {
    throw std::invalid_argument("Unsupported encoding in preferredFormats");
  }
#endif

  // check if preferredFormats has duplicates (forbidden)
  if (std::ranges::any_of(preferredFormats,
                          [this](Encoding enc) { return std::ranges::count(preferredFormats, enc) > 1; })) {
    throw std::invalid_argument("preferredFormats contains duplicate encodings");
  }

#ifdef AERONET_ENABLE_BROTLI
  if (brotli.quality < Brotli::kMinQuality || brotli.quality > Brotli::kMaxQuality) {
    throw std::invalid_argument("Invalid Brotli quality");
  }
  if (brotli.window < Brotli::kMinWindow || brotli.window > Brotli::kMaxWindow) {
    throw std::invalid_argument("Invalid Brotli window");
  }
#endif

#ifdef AERONET_ENABLE_ZLIB
  if (zlib.level != Zlib::kDefaultLevel && (zlib.level < Zlib::kMinLevel || zlib.level > Zlib::kMaxLevel)) {
    throw std::invalid_argument("Invalid ZLIB compression level");
  }
#endif

#ifdef AERONET_ENABLE_ZSTD
  assert(zstd.compressionLevel >= ZSTD_minCLevel());
  if (zstd.compressionLevel > ZSTD_maxCLevel()) {
    throw std::invalid_argument("Invalid ZSTD compression level");
  }
#endif
}

}  // namespace aeronet