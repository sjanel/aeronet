#pragma once

#include <cstddef>
#include <string_view>

#include "raw-chars.hpp"

namespace aeronet {

class ZstdDecoder {
 public:
  // Attempts to fully decompress input into out (append). Returns true on success; false on error
  // or if decompressed size would exceed maxDecompressedBytes. Uses an adaptive growth strategy
  // since the uncompressed size may be unknown (no content size header present in frame).
  static bool Decompress(std::string_view input, RawChars& out, std::size_t maxDecompressedBytes);
};

}  // namespace aeronet
