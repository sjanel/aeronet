#include "aeronet/compression-test-helpers.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

#include "aeronet/zlib-stream-raii.hpp"

#ifdef AERONET_ENABLE_BROTLI
#include "aeronet/brotli-decoder.hpp"
#include "aeronet/brotli-encoder.hpp"
#endif

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-decoder.hpp"
#include "aeronet/zlib-encoder.hpp"
#endif

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>

#include "aeronet/zstd-decoder.hpp"
#include "aeronet/zstd-encoder.hpp"
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

RawChars Compress(Encoding encoding, std::string_view payload) {
  RawChars compressed(payload.size() + 1024UL);

  switch (encoding) {
#ifdef AERONET_ENABLE_ZLIB
    case Encoding::gzip: {
      ZlibEncoder encoder(3);
      const auto written =
          encoder.encodeFull(ZStreamRAII::Variant::gzip, payload, compressed.capacity(), compressed.data());
      if (written == 0) {
        throw std::runtime_error("ZlibEncoder error");
      }
      compressed.setSize(static_cast<RawChars::size_type>(written));
      break;
    }
    case Encoding::deflate: {
      ZlibEncoder encoder(3);
      const auto written =
          encoder.encodeFull(ZStreamRAII::Variant::deflate, payload, compressed.capacity(), compressed.data());
      if (written == 0) {
        throw std::runtime_error("ZlibEncoder error");
      }
      compressed.setSize(static_cast<RawChars::size_type>(written));
      break;
    }
#endif
#ifdef AERONET_ENABLE_ZSTD
    case Encoding::zstd: {
      ZstdEncoder encoder(CompressionConfig::Zstd{});
      const auto written = encoder.encodeFull(payload, compressed.capacity(), compressed.data());
      if (written == 0) {
        throw std::runtime_error("ZstdEncoder error");
      }
      compressed.setSize(static_cast<RawChars::size_type>(written));
      break;
    }
#endif
#ifdef AERONET_ENABLE_BROTLI
    case Encoding::br: {
      BrotliEncoder encoder(CompressionConfig::Brotli{});
      const auto written = encoder.encodeFull(payload, compressed.capacity(), compressed.data());
      if (written == 0) {
        throw std::runtime_error("BrotliEncoder error");
      }
      compressed.setSize(static_cast<RawChars::size_type>(written));
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

void EndStream(EncoderContext& ctx, RawChars& out) {
  while (true) {
    out.ensureAvailableCapacityExponential(ctx.endChunkSize());
    const auto written = ctx.end(out.availableCapacity(), out.data() + out.size());
    if (written < 0) {
      out.clear();
      break;
    }
    if (written == 0) {
      break;
    }
    out.addSize(static_cast<RawChars::size_type>(written));
  }
}

RawChars BuildStreamingCompressed(EncoderContext& ctx, std::string_view payload, std::size_t split) {
  RawChars compressed;
  std::string_view remaining = payload;

  while (!remaining.empty()) {
    const std::size_t take = std::min(split, remaining.size());
    const auto chunk = remaining.substr(0, take);
    remaining.remove_prefix(take);

    // Reserve maximum possible compressed size for this chunk
    compressed.ensureAvailableCapacityExponential(ctx.maxCompressedBytes(chunk.size()));

    int64_t written = ctx.encodeChunk(chunk, compressed.availableCapacity(), compressed.data() + compressed.size());

    if (written < 0) {
      // Still failed, give up
      return {};
    }

    compressed.addSize(static_cast<RawChars::size_type>(written));
  }

  test::EndStream(ctx, compressed);

  return compressed;
}

}  // namespace aeronet::test
