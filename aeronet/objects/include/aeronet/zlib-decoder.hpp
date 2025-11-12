#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

// Minimal full-buffer zlib/gzip inflate helper used for inbound request decompression.
// Not exposed publicly; header installed only because internal components span static libs.
class ZlibDecoder {
 public:
  // Returns true on success; false if inflate fails or limits exceeded. Output appended to out.
  // isGzip selects window bits to enable gzip wrapper decoding.
  static bool Decompress(std::string_view input, bool isGzip, std::size_t maxDecompressedBytes,
                         std::size_t decoderChunkSize, RawChars &out);
};

}  // namespace aeronet
