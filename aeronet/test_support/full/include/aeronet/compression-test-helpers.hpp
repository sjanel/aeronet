#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/encoder-result.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/fixedcapacityvector.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

class EncoderContext;

namespace test {

// Decompress a single zstd frame contained in 'compressed'. If the frame size is known
// (via frame header) we trust it; otherwise we fall back to an expected size hint.
// expectedDecompressedSizeHint may be zero; in that case and when the frame size is
// unknown we return an empty string to signal inability (tests can decide how to handle).
std::string ZstdRoundTripDecompress(std::string_view compressed, std::size_t expectedDecompressedSizeHint = 0);

constexpr bool HasZstdMagic(std::string_view body) {
  // zstd frame magic little endian 0x28 B5 2F FD
  return body.size() >= 4 && static_cast<unsigned char>(body[0]) == 0x28 &&
         static_cast<unsigned char>(body[1]) == 0xB5 && static_cast<unsigned char>(body[2]) == 0x2F &&
         static_cast<unsigned char>(body[3]) == 0xFD;
}

std::string MakePatternedPayload(std::size_t size);

// Create a random payload of given size so that it's very difficult to compress.
RawChars MakeRandomPayload(std::size_t size);

RawChars MakeMixedPayload(std::size_t randomSize, std::size_t patternSize);

RawChars Compress(Encoding encoding, std::string_view payload);

RawChars Decompress(Encoding encoding, std::string_view compressed);

// Corrupt the compressed data in-place for the given encoding.
void CorruptData(Encoding encoding, RawChars& data);

EncoderResult EncodeChunk(EncoderContext& ctx, std::string_view data, RawChars& out);

void EndStream(EncoderContext& ctx, RawChars& out);

RawChars BuildStreamingCompressed(EncoderContext& ctx, std::string_view payload, std::size_t split);

// What FeedNonFinal() observed.
struct FedStream {
  RawChars decoded;
  std::vector<std::size_t> completeAt;  // input sizes after which an empty final chunk reported a complete stream
  bool ok{true};                        // every piece was decoded
};

// Feed `stream` in pieces of `split` bytes, none of them final, to the freshly initialized decoder context `ctx`,
// asking after each piece, with an empty final chunk, whether the input fed so far completes the stream.
template <class DecoderContext>
FedStream FeedNonFinal(DecoderContext& ctx, std::string_view stream, std::size_t split,
                       uint32_t decoderChunkSize = 4096U) {
  static constexpr std::size_t kNoLimit = std::numeric_limits<std::size_t>::max();
  FedStream res;
  for (std::size_t off = 0; off < stream.size();) {
    const std::string_view piece = stream.substr(off, split);
    off += piece.size();
    if (!ctx.decompressChunk(piece, false, kNoLimit, decoderChunkSize, res.decoded)) {
      res.ok = false;
      break;
    }
    if (ctx.decompressChunk({}, true, kNoLimit, decoderChunkSize, res.decoded)) {
      res.completeAt.push_back(off);
    }
  }
  return res;
}

inline auto SupportedEncodings() {
  FixedCapacityVector<Encoding, kNbContentEncodings> encs;
#ifdef AERONET_ENABLE_ZLIB
  encs.push_back(Encoding::gzip);
  encs.push_back(Encoding::deflate);
#endif
#ifdef AERONET_ENABLE_BROTLI
  encs.push_back(Encoding::br);
#endif
#ifdef AERONET_ENABLE_ZSTD
  encs.push_back(Encoding::zstd);
#endif
  return encs;
}

inline auto SupportedEncodingWithIdentity() {
  auto encs = SupportedEncodings();
  encs.push_back(Encoding::none);
  return encs;
}

}  // namespace test
}  // namespace aeronet
