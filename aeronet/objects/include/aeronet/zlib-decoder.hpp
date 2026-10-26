#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/raw-chars.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-stream-raii.hpp"
#include "aeronet/zlib-variant.hpp"

namespace aeronet {

class ZlibDecoderContext {
 public:
  ZlibDecoderContext() noexcept = default;

  // Feed a compressed chunk into the context.
  // When finalChunk is true, the caller does not provide any additional input: a stream left incomplete is an error.
  // An empty chunk decodes nothing: with finalChunk, it only tells whether the input given since init() completed the
  // stream (so a caller not knowing which chunk is the last one can signal the end of its input afterwards).
  // Returns true on success, false on failure (e.g. decompression error or exceeding maxDecompressedBytes).
  // maxDecompressedBytes bounds the bytes this call appends to out (the caller passes what is left of its budget):
  // exceeding it fails with more than maxDecompressedBytes bytes appended.
  bool decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                       uint32_t decoderChunkSize, RawChars& out) {
    return decompressChunk(chunk, finalChunk, maxDecompressedBytes, decoderChunkSize, out, kZMaxSize);
  }

  /// Initialize (or reinitialize) the decompression context with given parameters.
  /// Reuses internal zlib state if already initialized.
  void init(ZlibVariant variant) {
    _zs.initDecompress(variant);
    _streamEnded = false;
  }

 private:
  friend struct ZlibTestAccess;

  // zlib counts the input in 32 bits: the chunk is given to inflate() in slices of at most maxSliceSize bytes.
  bool decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                       uint32_t decoderChunkSize, RawChars& out, std::size_t maxSliceSize);

  ZStreamRAII _zs;
  bool _streamEnded{false};  // the end of the compressed stream was decoded
};

// Minimal full-buffer zlib/gzip inflate helper used for inbound request decompression.
// Not exposed publicly; header installed only because internal components span static libs.
class ZlibDecoder {
 public:
  explicit ZlibDecoder(ZlibVariant variant = ZlibVariant::gzip) noexcept : _variant(variant) {}

  bool decompressFull(std::string_view input, std::size_t maxDecompressedBytes, uint32_t decoderChunkSize,
                      RawChars& out) {
    return makeContext()->decompressChunk(input, true, maxDecompressedBytes, decoderChunkSize, out);
  }

  ZlibDecoderContext* makeContext() {
    _ctx.init(_variant);
    return &_ctx;
  }

  void setVariant(ZlibVariant variant) { _variant = variant; }

 private:
  ZlibDecoderContext _ctx;
  ZlibVariant _variant;
};

}  // namespace aeronet
