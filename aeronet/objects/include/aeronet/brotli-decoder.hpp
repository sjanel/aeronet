#pragma once

#include <cstddef>
#include <string_view>

#include "raw-chars.hpp"

namespace aeronet {

class BrotliDecoder {
 public:
  // Decompresses full brotli-encoded input into out. Returns true on success; false on error or size guard.
  static bool Decompress(std::string_view input, RawChars &out, std::size_t maxDecompressedBytes);
};

}  // namespace aeronet
