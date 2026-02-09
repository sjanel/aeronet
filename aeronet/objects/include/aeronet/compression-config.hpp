#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "aeronet/concatenated-strings.hpp"
#include "aeronet/direct-compression-mode.hpp"
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

  [[nodiscard]] std::size_t maxCompressedBytes(std::size_t uncompressedBytes) const {
    return static_cast<std::size_t>(std::ceil(static_cast<double>(uncompressedBytes) * maxCompressRatio));
  }

  // Preferred order of formats to negotiate (first supported & accepted wins).
  // If empty, defaults to enumeration order of Encoding.
  // Duplicates are not allowed, and it should contain a maximum of one of each encoding.
  FixedCapacityVector<Encoding, kNbContentEncodings - 1U> preferredFormats;

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

  // Default direct compression mode for HttpResponse. This will be used to set the initial direct compression mode of
  // HttpResponse instances, which can be overridden on a per-response basis by calling
  // HttpResponse::setDirectCompressionMode.
  DirectCompressionMode defaultDirectCompressionMode{DirectCompressionMode::Auto};

  // Automatic compression at the finalization of the HttpResponse is applied if and only if:
  //   compressedSize <= uncompressedSize * maxCompressRatio
  // This settings will be used to set a maximum compressed size when trying to apply automatic compression.
  // If the encoder produces compressed data larger than this threshold, compression will be aborted.
  // This is to avoid cases where compression can actually increase the size of the response, which can happen for very
  // small buffers and/or incompressible data. This is ignored for streaming compressions, because it's not possible to
  // know the compressed size beforehand.
  //
  // Example: 0.75 requires at least 25% savings. This value represents the maximum
  // allowed compressed/uncompressed ratio (i.e. compressedSize/uncompressedSize).
  // Should be in the range (0.0, 1.0), exclusive.
  float maxCompressRatio{0.5};

  // Only responses whose (uncompressed) size is >= this threshold are considered for compression.
  // For streaming handlers responses (unknown size), compression begins once cumulative bytes reach threshold.
  // For direct automatic compression, only responses whose first body chunk size is >= this threshold are considered
  // for compression. You can set this threshold to std::numeric_limits<std::size_t>::max() to effectively disable
  // automatic compression as all responses will be considered smaller than the threshold.
  std::size_t minBytes{1024U};

  // List of content-types eligible for response body compression.
  // If empty, any content type will be eligible for compression.
  // If the server mixes compressible and non-compressible content types (for instance, jpeg is non-compressible, but
  // json usually is), it is recommended to set this allowlist to avoid wasting CPU cycles trying to compress already
  // small or incompressible responses.
  ConcatenatedStrings32 contentTypeAllowList;
};

}  // namespace aeronet
