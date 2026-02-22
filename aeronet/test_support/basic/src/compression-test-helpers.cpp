#include "aeronet/compression-test-helpers.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoder-result.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/raw-chars.hpp"

#ifdef AERONET_ENABLE_BROTLI
#include "aeronet/brotli-decoder.hpp"
#include "aeronet/brotli-encoder.hpp"
#endif

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-decoder.hpp"
#include "aeronet/zlib-encoder.hpp"
#include "aeronet/zlib-stream-raii.hpp"
#endif

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>

#include "aeronet/zstd-decoder.hpp"
#include "aeronet/zstd-encoder.hpp"
#endif

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

RawChars MakeRandomPayload(std::size_t size) {
  RawChars payload(size);
  std::mt19937_64 rng{123456789ULL};
  std::uniform_int_distribution<char> dist(std::numeric_limits<char>::min(), std::numeric_limits<char>::max());
  for (std::size_t i = 0; i < size; ++i) {
    payload[i] = dist(rng);
  }
  payload.setSize(size);
  return payload;
}

RawChars MakeMixedPayload(std::size_t randomSize, std::size_t patternSize) {
  RawChars random = MakeRandomPayload(randomSize);
  std::string pattern = MakePatternedPayload(patternSize);
  RawChars mixed(random.size() + pattern.size());
  mixed.unchecked_append(random.data(), random.size());
  mixed.unchecked_append(pattern.data(), pattern.size());
  return mixed;
}

RawChars Compress(Encoding encoding, std::string_view payload) {
  RawChars compressed(payload.size() + 1024UL);

  switch (encoding) {
#ifdef AERONET_ENABLE_ZLIB
    case Encoding::gzip: {
      ZlibEncoder encoder(3);
      const auto result =
          encoder.encodeFull(ZStreamRAII::Variant::gzip, payload, compressed.capacity(), compressed.data());
      if (result.hasError()) {
        throw std::runtime_error("ZlibEncoder error");
      }
      compressed.setSize(result.written());
      break;
    }
    case Encoding::deflate: {
      ZlibEncoder encoder(3);
      const auto result =
          encoder.encodeFull(ZStreamRAII::Variant::deflate, payload, compressed.capacity(), compressed.data());
      if (result.hasError()) {
        throw std::runtime_error("ZlibEncoder error");
      }
      compressed.setSize(result.written());
      break;
    }
#endif
#ifdef AERONET_ENABLE_ZSTD
    case Encoding::zstd: {
      ZstdEncoder encoder(CompressionConfig::Zstd{});
      const auto result = encoder.encodeFull(payload, compressed.capacity(), compressed.data());
      if (result.hasError()) {
        throw std::runtime_error("ZstdEncoder error");
      }
      compressed.setSize(result.written());
      break;
    }
#endif
#ifdef AERONET_ENABLE_BROTLI
    case Encoding::br: {
      BrotliEncoder encoder(CompressionConfig::Brotli{});
      const auto result = encoder.encodeFull(payload, compressed.capacity(), compressed.data());
      if (result.hasError()) {
        throw std::runtime_error("BrotliEncoder error");
      }
      compressed.setSize(result.written());
      break;
    }
#endif
    case Encoding::none:
      compressed.assign(payload.data(), payload.size());
      break;
    default:
      throw std::invalid_argument("Unsupported encoding");
  }
  return compressed;
}

RawChars Decompress(Encoding encoding, std::string_view compressed) {
  RawChars decompressed;

  switch (encoding) {
#ifdef AERONET_ENABLE_ZLIB
    case Encoding::gzip: {
      ZlibDecoder decoder(ZStreamRAII::Variant::gzip);
      if (!decoder.decompressFull(compressed, std::numeric_limits<std::size_t>::max(), 1024, decompressed)) {
        throw std::runtime_error("ZlibDecoder error gzip");
      }
      break;
    }
    case Encoding::deflate: {
      ZlibDecoder decoder(ZStreamRAII::Variant::deflate);
      if (!decoder.decompressFull(compressed, std::numeric_limits<std::size_t>::max(), 1024, decompressed)) {
        throw std::runtime_error("ZlibDecoder error deflate");
      }
      break;
    }
#endif
#ifdef AERONET_ENABLE_ZSTD
    case Encoding::zstd: {
      ZstdDecoder decoder;
      if (!decoder.decompressFull(compressed, std::numeric_limits<std::size_t>::max(), 1024, decompressed)) {
        throw std::runtime_error("ZstdDecoder error");
      }
      break;
    }
#endif
#ifdef AERONET_ENABLE_BROTLI
    case Encoding::br: {
      BrotliDecoder decoder;
      if (!decoder.decompressFull(compressed, std::numeric_limits<std::size_t>::max(), 1024, decompressed)) {
        throw std::runtime_error("BrotliDecoder error");
      }
      break;
    }
#endif
    case Encoding::none:
      decompressed.assign(compressed.data(), compressed.size());
      break;
    default:
      throw std::invalid_argument("Unsupported encoding");
  }
  return decompressed;
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

EncoderResult EncodeChunk(EncoderContext& ctx, std::string_view data, RawChars& out) {
  out.clear();
  out.reserve(ctx.minEncodeChunkCapacity(data.size()));
  const auto result = ctx.encodeChunk(data, out.capacity(), out.data());
  if (!result.hasError()) {
    out.setSize(result.written());
  }
  return result;
}

void EndStream(EncoderContext& ctx, RawChars& out) {
  while (true) {
    out.ensureAvailableCapacityExponential(ctx.endChunkSize());
    const auto result = ctx.end(out.availableCapacity(), out.data() + out.size());
    if (result.hasError()) {
      out.clear();
      break;
    }
    if (result.written() == 0) {
      break;
    }
    out.addSize(result.written());
  }
}

RawChars BuildStreamingCompressed(EncoderContext& ctx, std::string_view payload, std::size_t split) {
  RawChars compressed;
  std::string_view remaining = payload;

  while (!remaining.empty()) {
    const std::size_t take = std::min(split, remaining.size());
    const auto chunk = remaining.substr(0, take);
    remaining.remove_prefix(take);

    // Reserve minimum possible compressed size for this chunk
    compressed.ensureAvailableCapacityExponential(ctx.minEncodeChunkCapacity(chunk.size()));

    const auto result = ctx.encodeChunk(chunk, compressed.availableCapacity(), compressed.data() + compressed.size());

    if (result.hasError()) {
      // Still failed, give up
      throw std::runtime_error("Encoding chunk failed");
      return {};
    }

    compressed.addSize(result.written());
  }

  test::EndStream(ctx, compressed);

  return compressed;
}

}  // namespace aeronet::test
