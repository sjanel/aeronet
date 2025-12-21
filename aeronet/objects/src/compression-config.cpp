#include "aeronet/compression-config.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>

#include "aeronet/encoding.hpp"
#include "aeronet/features.hpp"

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>

#include "aeronet/zstd-encoder.hpp"
#endif

namespace aeronet {

void CompressionConfig::validate() const {
  if (encoderChunkSize == 0) {
    throw std::invalid_argument("Invalid encoder chunk size");
  }
  if constexpr (aeronet::zlibEnabled()) {
    if (zlib.level != Zlib::kDefaultLevel && (zlib.level < Zlib::kMinLevel || zlib.level > Zlib::kMaxLevel)) {
      throw std::invalid_argument(std::format("Invalid ZLIB compression level {}", zlib.level));
    }
  }
  auto it = std::ranges::find_if_not(preferredFormats, [](Encoding enc) { return IsEncodingEnabled(enc); });
  if (it != preferredFormats.end()) {
    throw std::invalid_argument(std::format("Unsupported encoding {} in preferredFormats", static_cast<int>(*it)));
  }

#ifdef AERONET_ENABLE_ZSTD
  if (zstd.compressionLevel < ZSTD_minCLevel() || zstd.compressionLevel > ZSTD_maxCLevel()) {
    throw std::invalid_argument(std::format("Invalid ZSTD compression level {}", zstd.compressionLevel));
  }
  details::ZstdContextRAII testConstruction(zstd.compressionLevel, zstd.windowLog);
#endif

  if constexpr (aeronet::brotliEnabled()) {
    if (brotli.quality < Brotli::kMinQuality || brotli.quality > Brotli::kMaxQuality) {
      throw std::invalid_argument(std::format("Invalid Brotli quality {}", brotli.quality));
    }
    if (brotli.window < Brotli::kMinWindow || brotli.window > Brotli::kMaxWindow) {
      throw std::invalid_argument(std::format("Invalid Brotli window {}", brotli.window));
    }
  }
}

}  // namespace aeronet