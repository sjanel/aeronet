#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/buffer-cache.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

class BrotliDecoderContext {
 public:
  BrotliDecoderContext() noexcept = default;

  BrotliDecoderContext(const BrotliDecoderContext&) = delete;
  BrotliDecoderContext& operator=(const BrotliDecoderContext&) = delete;
  BrotliDecoderContext(BrotliDecoderContext&& rhs) noexcept;
  BrotliDecoderContext& operator=(BrotliDecoderContext&& rhs) noexcept;

  ~BrotliDecoderContext();

  // Feed a compressed chunk into the context.
  // When finalChunk is true, the caller does not provide any additional input: a stream left incomplete is an error.
  // An empty chunk decodes nothing: with finalChunk, it only tells whether the input given since init() completed the
  // stream (so a caller not knowing which chunk is the last one can signal the end of its input afterwards).
  // Returns true on success, false on failure (e.g. decompression error or exceeding maxDecompressedBytes).
  // maxDecompressedBytes bounds the bytes this call appends to out (the caller passes what is left of its budget):
  // exceeding it fails with more than maxDecompressedBytes bytes appended.
  bool decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                       uint32_t decoderChunkSize, RawChars& out);

  void init();

 private:
  internal::BufferCache _cache;  // MUST be before _pState so it's destroyed AFTER (inverse order)
  void* _pState{};
};

class BrotliDecoder {
 public:
  bool decompressFull(std::string_view input, std::size_t maxDecompressedBytes, uint32_t decoderChunkSize,
                      RawChars& out) {
    _ctx.init();  // Reset decoder before decompression
    return _ctx.decompressChunk(input, true, maxDecompressedBytes, decoderChunkSize, out);
  }

  BrotliDecoderContext* makeContext() {
    _ctx.init();
    return &_ctx;
  }

 private:
  BrotliDecoderContext _ctx;
};

}  // namespace aeronet
