#include "aeronet/compression-test-helpers.hpp"

#include <random>

#include "aeronet/raw-bytes.hpp"

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>

#include <stdexcept>
#endif

#include <cstddef>
#include <numeric>
#include <string>
#include <string_view>

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

}  // namespace aeronet::test
