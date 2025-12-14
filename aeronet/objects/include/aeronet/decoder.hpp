#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

// Streaming decoder abstraction mirroring Encoder/EncoderContext.
// Implementations may reuse internal buffers but are not thread-safe.
class DecoderContext {
 public:
  virtual ~DecoderContext() = default;

  // Feed a compressed chunk into the context.
  // When finalChunk is true, the caller does not provide any additional input.
  // Implementations append plain bytes to 'out'.
  // Returns true on success, false on failure (e.g. decompression error or exceeding maxDecompressedBytes).
  virtual bool decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                               std::size_t decoderChunkSize, RawChars &out) = 0;
};

class Decoder {
 public:
  virtual ~Decoder() = default;

  // Convenience helper for full-buffer decompression.
  virtual bool decompressFull(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                              RawChars &out) = 0;

  // Create a streaming context for incremental decode.
  virtual std::unique_ptr<DecoderContext> makeContext() = 0;
};

}  // namespace aeronet
