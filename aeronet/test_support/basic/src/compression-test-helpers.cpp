#include "aeronet/compression-test-helpers.hpp"

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>
#endif

#include "aeronet/encoder.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet::test {

// Decompress a single zstd frame contained in 'compressed'. If the frame size is known
// (via frame header) we trust it; otherwise we fall back to an expected size hint.
// expectedDecompressedSizeHint may be zero; in that case and when the frame size is
// unknown we return an empty string to signal inability (tests can decide how to handle).
std::string ZstdRoundTripDecompress([[maybe_unused]] std::string_view compressed,
                                    [[maybe_unused]] std::size_t expectedDecompressedSizeHint) {
  std::string out;
#ifdef AERONET_ENABLE_ZSTD
  if (compressed.empty()) {
    return out;
  }
  const std::size_t frameSize = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
  if (frameSize != ZSTD_CONTENTSIZE_ERROR && frameSize != ZSTD_CONTENTSIZE_UNKNOWN) {
    out.assign(frameSize, '\0');
    const std::size_t dsz = ZSTD_decompress(out.data(), out.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(dsz) == 1) {
      throw std::runtime_error("ZSTD decompress error");
    }
    out.resize(dsz);
    return out;
  }
  if (expectedDecompressedSizeHint == 0) {
    out.clear();
    return out;  // insufficient information
  }
  out.assign(expectedDecompressedSizeHint, '\0');
  const std::size_t dsz = ZSTD_decompress(out.data(), out.size(), compressed.data(), compressed.size());
  if (ZSTD_isError(dsz) == 1) {
    throw std::runtime_error("ZSTD decompress error");
  }
  out.resize(dsz);
#endif
  return out;
}

std::string MakePatternedPayload(std::size_t size) {
  std::string payload;
  payload.resize_and_overwrite(size, [](char* data, std::size_t size) {
    std::iota(data, data + size, static_cast<unsigned char>(0));
    return size;
  });
  return payload;
}

RawBytes MakeRandomPayload(std::size_t size) {
  RawBytes payload(size);
  std::mt19937_64 rng{123456789ULL};
  std::uniform_int_distribution<int> dist(0, 255);
  for (std::size_t i = 0; i < size; ++i) {
    payload[i] = static_cast<std::byte>(dist(rng));
  }
  payload.setSize(size);
  return payload;
}

void CorruptData(std::string_view encoding, RawChars& data) {
  if (encoding == http::gzip || encoding == http::deflate) {
    if (data.size() < 6) {
      throw std::invalid_argument("Data too small to corrupt for gzip/deflate");
    }
    // Remove trailing bytes (part of CRC/ISIZE) to induce inflate failure.
    data.setSize(data.size() - 6);
  } else if (encoding == http::zstd) {
    if (data.size() < 4) {
      throw std::invalid_argument("Data too small to corrupt for zstd");
    }
    // Flip all bits of first byte of magic number via unsigned char to avoid -Wconversion warning
    unsigned char* bytePtr = reinterpret_cast<unsigned char*>(data.data());
    bytePtr[0] ^= 0xFFU;  // corrupt magic (0x28 -> ~0x28)
  } else if (encoding == http::br) {
    if (data.size() < 8) {
      throw std::invalid_argument("Data too small to corrupt for brotli");
    }
    // Truncate last 4 bytes to corrupt brotli stream
    data.setSize(data.size() - 4);
  } else {
    throw std::invalid_argument("Unsupported encoding for corruption");
  }
}

int64_t EncodeChunk(EncoderContext& ctx, std::string_view data, RawChars& out) {
  out.clear();
  out.reserve(ctx.maxCompressedBytes(data.size()));
  const auto written = ctx.encodeChunk(data, out.capacity(), out.data());
  if (written > 0) {
    out.setSize(static_cast<RawChars::size_type>(written));
  }
  return written;
}

RawChars EndStream(EncoderContext& ctx) {
  RawChars out;
  while (true) {
    out.ensureAvailableCapacityExponential(ctx.endChunkSize());
    const auto written = ctx.end(out.availableCapacity(), out.data() + out.size());
    if (written < 0) {
      out.clear();
      return out;
    }
    if (written == 0) {
      break;
    }
    out.addSize(static_cast<RawChars::size_type>(written));
  }
  return out;
}

RawChars BuildStreamingCompressed(EncoderContext& ctx, std::string_view payload, std::size_t split) {
  RawChars compressed;
  std::string_view remaining = payload;

  while (!remaining.empty()) {
    const std::size_t take = std::min(split, remaining.size());
    const auto chunk = remaining.substr(0, take);
    remaining.remove_prefix(take);

    RawChars chunkOut;
    // Reserve maximum possible compressed size for this chunk
    chunkOut.reserve(ctx.maxCompressedBytes(chunk.size()));

    int64_t written = ctx.encodeChunk(chunk, chunkOut.capacity(), chunkOut.data());

    // If insufficient output buffer, try with more space
    if (written < 0) {
      chunkOut.ensureAvailableCapacityExponential(ctx.maxCompressedBytes(chunk.size()) * 2);
      written = ctx.encodeChunk(chunk, chunkOut.capacity(), chunkOut.data());
    }

    if (written < 0) {
      // Still failed, give up
      return {};
    }

    if (written > 0) {
      chunkOut.setSize(static_cast<RawChars::size_type>(written));
      compressed.append(chunkOut);
    }
  }

  const auto tail = test::EndStream(ctx);
  if (!tail.empty()) {
    compressed.append(tail);
  }

  return compressed;
}

}  // namespace aeronet::test
