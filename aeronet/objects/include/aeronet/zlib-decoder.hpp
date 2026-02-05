#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/raw-chars.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

class ZlibDecoderContext {
 public:
  ZlibDecoderContext() noexcept = default;

  // Feed a compressed chunk into the context.
  // When finalChunk is true, the caller does not provide any additional input.
  // Returns true on success, false on failure (e.g. decompression error or exceeding maxDecompressedBytes).
  bool decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                       std::size_t decoderChunkSize, RawChars &out);

  /// Initialize (or reinitialize) the decompression context with given parameters.
  /// Reuses internal zlib state if already initialized.
  void init(ZStreamRAII::Variant variant) { _zs.initDecompress(variant); }

 private:
  ZStreamRAII _zs;
};

// Minimal full-buffer zlib/gzip inflate helper used for inbound request decompression.
// Not exposed publicly; header installed only because internal components span static libs.
class ZlibDecoder {
 public:
  explicit ZlibDecoder(ZStreamRAII::Variant variant = ZStreamRAII::Variant::gzip) noexcept : _variant(variant) {}

  bool decompressFull(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                      RawChars &out) {
    return makeContext()->decompressChunk(input, true, maxDecompressedBytes, decoderChunkSize, out);
  }

  ZlibDecoderContext *makeContext() {
    _ctx.init(_variant);
    return &_ctx;
  }

  void setVariant(ZStreamRAII::Variant variant) { _variant = variant; }

 private:
  ZlibDecoderContext _ctx;
  ZStreamRAII::Variant _variant;
};

}  // namespace aeronet
