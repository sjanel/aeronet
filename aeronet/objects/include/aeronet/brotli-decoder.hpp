#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

class BrotliStreamingContext {
 public:
  BrotliStreamingContext();

  BrotliStreamingContext(const BrotliStreamingContext &) = delete;
  BrotliStreamingContext &operator=(const BrotliStreamingContext &) = delete;
  BrotliStreamingContext(BrotliStreamingContext &&) noexcept = delete;
  BrotliStreamingContext &operator=(BrotliStreamingContext &&) noexcept = delete;

  ~BrotliStreamingContext();

  // Feed a compressed chunk into the context.
  // When finalChunk is true, the caller does not provide any additional input.
  // Returns true on success, false on failure (e.g. decompression error or exceeding maxDecompressedBytes).
  bool decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                       std::size_t decoderChunkSize, RawChars &out);

 private:
  void *_pState;
};

class BrotliDecoder {
 public:
  static bool decompressFull(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                             RawChars &out) {
    return makeContext().decompressChunk(input, true, maxDecompressedBytes, decoderChunkSize, out);
  }

  static BrotliStreamingContext makeContext() { return BrotliStreamingContext{}; }
};

}  // namespace aeronet
