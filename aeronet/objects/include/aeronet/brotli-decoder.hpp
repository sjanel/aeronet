#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "aeronet/decoder.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

class BrotliDecoder : public Decoder {
 public:
  // Decompresses full brotli-encoded input into out. Returns true on success; false on error or size guard.
  static bool Decompress(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                         RawChars &out);

  bool decompressFull(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                      RawChars &out) override;

  std::unique_ptr<DecoderContext> makeContext() override;
};

}  // namespace aeronet
