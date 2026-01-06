#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/raw-chars.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

class ZlibStreamingContext {
 public:
  explicit ZlibStreamingContext(bool isGzip)
      : _context(isGzip ? ZStreamRAII::Variant::gzip : ZStreamRAII::Variant::deflate) {}

  // Feed a compressed chunk into the context.
  // When finalChunk is true, the caller does not provide any additional input.
  // Returns true on success, false on failure (e.g. decompression error or exceeding maxDecompressedBytes).
  bool decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                       std::size_t decoderChunkSize, RawChars &out);

 private:
  ZStreamRAII _context;
};

// Minimal full-buffer zlib/gzip inflate helper used for inbound request decompression.
// Not exposed publicly; header installed only because internal components span static libs.
class ZlibDecoder {
 public:
  explicit ZlibDecoder(bool isGzip) : _isGzip(isGzip) {}

  bool decompressFull(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                      RawChars &out) const {
    return makeContext().decompressChunk(input, true, maxDecompressedBytes, decoderChunkSize, out);
  }

  [[nodiscard]] ZlibStreamingContext makeContext() const { return ZlibStreamingContext{_isGzip}; }

 private:
  bool _isGzip;
};

}  // namespace aeronet
