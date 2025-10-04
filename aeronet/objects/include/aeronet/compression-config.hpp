#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "encoding.hpp"
#include "fixedcapacityvector.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include <zlib.h>
#endif

namespace aeronet {

// NOTE: Compression is optional at build time. When AERONET_ENABLE_ZLIB is not defined, only the
// Format::None mode is honored; attempts to select gzip/deflate should be ignored or cause a
// graceful fallback. Additional formats (brotli, zstd) are added behind their own build flags.
struct CompressionConfig {
  // Preferred order of formats to negotiate (first supported & accepted wins). If empty, defaults
  // to enumeration order of Encoding.
  FixedCapacityVector<Encoding, kNbContentEncodings> preferredFormats;

  struct Zlib {
#ifdef AERONET_ENABLE_ZLIB
    int8_t level = Z_DEFAULT_COMPRESSION;
#else
    int8_t level = 0;
#endif
  } zlib;

  struct Zstd {
#ifdef AERONET_ENABLE_ZSTD
    int compressionLevel = 3;  // reasonable default
    int windowLog = 0;         // 0 -> library default
#else
    int compressionLevel = 0;
    int windowLog = 0;
#endif
  } zstd;

  struct Brotli {
#ifdef AERONET_ENABLE_BROTLI
    int quality = 5;  // 0-11 (11 slowest/best). Choose balanced default.
    int window = 0;   // 0 -> library default (implies 22 usually)
#else
    int quality = 0;
    int window = 0;
#endif
  } brotli;

  // Only responses whose (uncompressed) size is >= this threshold are considered for compression.
  // For streaming responses (unknown size), compression begins once cumulative bytes reach threshold.
  std::size_t minBytes{256};

  // Simple allowlist of content-types (prefix match) eligible for compression. If empty, a default
  // internal allowlist like: { "text/", "application/json", "application/javascript", "application/xml" }
  // will be applied at negotiation time.
  std::vector<std::string> contentTypeAllowlist;

  // If true, adds/merges a Vary: Accept-Encoding header whenever compression is applied.
  bool addVaryHeader{true};

  // Hard cap on the size of an internal staging buffer used by encoders (e.g. gzip). Prevents
  // excessive memory growth for highly compressible large streams. Default 64 KiB.
  std::size_t maxEncoderBufferBytes{64UL * 1024UL};

  // Whether to allow per-response opt-out via an API (e.g. response.disableCompression()).
  bool allowPerResponseDisable{true};
};

}  // namespace aeronet
