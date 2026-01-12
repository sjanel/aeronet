#pragma once

#include <cstddef>
#include <cstdint>

#include "aeronet/concatenated-strings.hpp"
#include "aeronet/fixedcapacityvector.hpp"
#include "encoding.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include <zlib.h>
#endif

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>
#endif

#ifdef AERONET_ENABLE_BROTLI
#include <brotli/encode.h>
#endif

namespace aeronet {

struct CompressionConfig {
  void validate() const;

  // Preferred order of formats to negotiate (first supported & accepted wins). If empty, defaults
  // to enumeration order of Encoding.
  FixedCapacityVector<Encoding, kNbContentEncodings> preferredFormats;

  // If true, adds/merges a Vary: Accept-Encoding header whenever compression is applied.
  bool addVaryAcceptEncodingHeader{true};

  struct Zlib {
#ifdef AERONET_ENABLE_ZLIB
    static constexpr int8_t kDefaultLevel = Z_DEFAULT_COMPRESSION;
    static constexpr int8_t kMinLevel = Z_BEST_SPEED;
    static constexpr int8_t kMaxLevel = Z_BEST_COMPRESSION;
#else
    static constexpr int8_t kDefaultLevel = 0;
    static constexpr int8_t kMinLevel = 0;
    static constexpr int8_t kMaxLevel = 0;
#endif
    int8_t level = kDefaultLevel;
  } zlib;

  struct Zstd {
#ifdef AERONET_ENABLE_ZSTD
    int8_t compressionLevel = ZSTD_CLEVEL_DEFAULT;
#else
    int8_t compressionLevel = 0;
#endif
    int8_t windowLog = 0;
  } zstd;

  struct Brotli {
#ifdef AERONET_ENABLE_BROTLI
    static constexpr int8_t kDefaultQuality = BROTLI_DEFAULT_QUALITY;
    static constexpr int8_t kDefaultWindow = BROTLI_DEFAULT_WINDOW;
    static constexpr int8_t kMinQuality = BROTLI_MIN_QUALITY;
    static constexpr int8_t kMaxQuality = BROTLI_MAX_QUALITY;
    static constexpr int8_t kMinWindow = BROTLI_MIN_WINDOW_BITS;
    static constexpr int8_t kMaxWindow = BROTLI_MAX_WINDOW_BITS;
#else
    static constexpr int8_t kDefaultQuality = 0;
    static constexpr int8_t kDefaultWindow = 0;
    static constexpr int8_t kMinQuality = 0;
    static constexpr int8_t kMaxQuality = 0;
    static constexpr int8_t kMinWindow = 0;
    static constexpr int8_t kMaxWindow = 0;
#endif
    int8_t quality = kDefaultQuality;
    int8_t window = kDefaultWindow;
  } brotli;

  // Only responses whose (uncompressed) size is >= this threshold are considered for compression.
  // For streaming responses (unknown size), compression begins once cumulative bytes reach threshold.
  std::size_t minBytes{1024U};

  // Compression is applied only if:
  //   compressedSize <= uncompressedSize * minCompressRatio
  // and the compressed size is strictly smaller than the original size.
  //
  // Example: 0.95 requires at least 5% savings.
  // Should be in the range (0.0, 1.0), exclusive.
  float minCompressRatio{0.5};

  // Simple allowlist of content-types (prefix match) eligible for compression. If empty, any content type will be
  // eligible for compression.
  ConcatenatedStrings32 contentTypeAllowList;
};

}  // namespace aeronet
