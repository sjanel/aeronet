#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "aeronet/decoder.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

// Minimal full-buffer zlib/gzip inflate helper used for inbound request decompression.
// Not exposed publicly; header installed only because internal components span static libs.
class ZlibDecoder : public Decoder {
 public:
  explicit ZlibDecoder(bool isGzip) : _isGzip(isGzip) {}

  // Returns true on success; false if inflate fails or limits exceeded. Output appended to out.
  // isGzip selects window bits to enable gzip wrapper decoding.
  static bool Decompress(std::string_view input, bool isGzip, std::size_t maxDecompressedBytes,
                         std::size_t decoderChunkSize, RawChars &out);

  bool decompressFull(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                      RawChars &out) override;

  std::unique_ptr<DecoderContext> makeContext() override;

 private:
  bool _isGzip{false};
};

}  // namespace aeronet
