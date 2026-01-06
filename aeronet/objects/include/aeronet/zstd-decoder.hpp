#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

class ZstdStreamingContext {
 public:
  ZstdStreamingContext();

  ZstdStreamingContext(const ZstdStreamingContext &) = delete;
  ZstdStreamingContext &operator=(const ZstdStreamingContext &) = delete;
  ZstdStreamingContext(ZstdStreamingContext &&) noexcept = delete;
  ZstdStreamingContext &operator=(ZstdStreamingContext &&) noexcept = delete;

  ~ZstdStreamingContext();

  // Feed a compressed chunk into the context.
  // When finalChunk is true, the caller does not provide any additional input.
  // Returns true on success, false on failure (e.g. decompression error or exceeding maxDecompressedBytes).
  bool decompressChunk(std::string_view chunk, [[maybe_unused]] bool finalChunk, std::size_t maxDecompressedBytes,
                       std::size_t decoderChunkSize, RawChars &out);

 private:
  void *_pState;
};

class ZstdDecoder {
 public:
  // Attempts to fully decompress input into out (append). Returns true on success; false on error
  // or if decompressed size would exceed maxDecompressedBytes. Uses an adaptive growth strategy
  // since the uncompressed size may be unknown (no content size header present in frame).
  static bool decompressFull(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                             RawChars &out);

  static ZstdStreamingContext makeContext() { return ZstdStreamingContext{}; }
};

}  // namespace aeronet
