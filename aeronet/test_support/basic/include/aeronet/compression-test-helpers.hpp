#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "aeronet/raw-bytes.hpp"
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
RawBytes MakeRandomPayload(std::size_t size);

// Corrupt the compressed data in-place for the given encoding.
void CorruptData(std::string_view encoding, RawChars& data);

int64_t EncodeChunk(EncoderContext& ctx, std::string_view data, RawChars& out);

void EndStream(EncoderContext& ctx, RawChars& out);

RawChars BuildStreamingCompressed(EncoderContext& ctx, std::string_view payload, std::size_t split);

}  // namespace test
}  // namespace aeronet
