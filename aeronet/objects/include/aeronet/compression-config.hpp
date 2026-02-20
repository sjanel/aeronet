#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "aeronet/concatenated-strings.hpp"
#include "aeronet/direct-compression-mode.hpp"
#include "aeronet/fixedcapacityvector.hpp"
#include "encoding.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-gateway.hpp"
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

  // Server-side preference order used to break ties during
  // Accept-Encoding negotiation.
  //
  // The client q-value always takes precedence. When multiple
  // encodings share the same effective q-value, this list
  // determines the winner (first match wins).
  //
  // If empty, the default enumeration order of Encoding is used.
  //
  // Each encoding may appear at most once.
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

  // Maximum allowed compressed size ratio relative to the
  // uncompressed body size.
  //
  // Automatic compression is applied only if:
  //
  //   compressedSize <= uncompressedSize * maxCompressRatio
  //
  // If compression would exceed this bound, the operation is
  // aborted and the response remains unmodified.
  //
  // This prevents size expansion on small or incompressible
  // payloads.
  //
  // Must be in the range (0.0, 1.0).
  //
  // Example: 0.6 requires at least 40% size reduction.
  float maxCompressRatio{0.6F};

  // Minimum uncompressed body size required before compression is considered.
  //
  // • For finalized (non-streaming) responses, compression is attempted only if total body size >= minBytes.
  //
  // • For streaming handlers responses (HttpResponseWriter) with unknown total size,
  //   compression activates once cumulative bytes reach this threshold.
  //
  // • For direct compression, the first inline body chunk must satisfy this threshold (unless
  //   DirectCompressionMode::On).
  //
  // Set to std::numeric_limits<std::size_t>::max() to effectively disable automatic compression.
  std::size_t minBytes{1024U};

  // Optional allow-list of content types eligible for compression.
  //
  // If empty, all content types are considered eligible.
  //
  // It is recommended to restrict this list when serving a mix of compressible (e.g., JSON, HTML) and non-compressible
  // content (e.g., JPEG, MP4) to avoid unnecessary CPU usage.
  ConcatenatedStrings32 contentTypeAllowList;
};

}  // namespace aeronet
