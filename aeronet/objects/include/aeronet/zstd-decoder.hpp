#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/decoder.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

class ZstdDecoder : public Decoder {
 public:
  // Attempts to fully decompress input into out (append). Returns true on success; false on error
  // or if decompressed size would exceed maxDecompressedBytes. Uses an adaptive growth strategy
  // since the uncompressed size may be unknown (no content size header present in frame).
  static bool Decompress(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                         RawChars &out);

  bool decompressFull(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                      RawChars &out) override;

  std::unique_ptr<DecoderContext> makeContext() override;
};

}  // namespace aeronet
