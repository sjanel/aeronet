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

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>
#endif

#ifdef AERONET_ENABLE_BROTLI
#include <brotli/encode.h>
#endif

namespace aeronet {

// NOTE: Compression is optional at build time. When AERONET_ENABLE_ZLIB is not defined, only the
// Format::None mode is honored; attempts to select gzip/deflate should be ignored or cause a
// graceful fallback. Additional formats (brotli, zstd) are added behind their own build flags.
struct CompressionConfig {
  void validate() const;

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
    int compressionLevel = ZSTD_CLEVEL_DEFAULT;
    int windowLog = 0;  // 0 -> library default
#else
    int compressionLevel = 0;
    int windowLog = 0;
#endif
  } zstd;

  struct Brotli {
#ifdef AERONET_ENABLE_BROTLI
    int quality = BROTLI_DEFAULT_QUALITY;  // 0-11 (11 slowest/best)
    int window = BROTLI_DEFAULT_WINDOW;
#else
    int quality = 0;
    int window = 0;
#endif
  } brotli;

  // Only responses whose (uncompressed) size is >= this threshold are considered for compression.
  // For streaming responses (unknown size), compression begins once cumulative bytes reach threshold.
  std::size_t minBytes{256};

  // Simple allowlist of content-types (prefix match) eligible for compression. If empty, any content type will be
  // eligible for compression.
  std::vector<std::string> contentTypeAllowlist;

  // If true, adds/merges a Vary: Accept-Encoding header whenever compression is applied.
  bool addVaryHeader{true};

  // Chunk size of buffer growths during compression.
  // Prefer a large size if you expect big payloads in average, prefer a small size if you want to limit memory
  // overhead.
  std::size_t encoderChunkSize{32UL * 1024UL};
};

}  // namespace aeronet
