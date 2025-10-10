#pragma once

#include <gtest/gtest.h>
#include <zstd.h>

#include <cassert>
#include <cstddef>
#include <string>
#include <string_view>

namespace aeronet::test {

// Decompress a single zstd frame contained in 'compressed'. If the frame size is known
// (via frame header) we trust it; otherwise we fall back to an expected size hint.
// expectedDecompressedSizeHint may be zero; in that case and when the frame size is
// unknown we return an empty string to signal inability (tests can decide how to handle).
inline std::string zstdRoundTripDecompress(std::string_view compressed, std::size_t expectedDecompressedSizeHint = 0) {
  std::string out;
  if (compressed.empty()) {
    return out;
  }
  std::size_t frameSize = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
  if (frameSize != ZSTD_CONTENTSIZE_ERROR && frameSize != ZSTD_CONTENTSIZE_UNKNOWN) {
    out.assign(frameSize, '\0');
    std::size_t const dsz = ZSTD_decompress(out.data(), out.size(), compressed.data(), compressed.size());
    EXPECT_NE(ZSTD_isError(dsz), 1);
    out.resize(dsz);
    return out;
  }
  if (expectedDecompressedSizeHint == 0) {
    out.clear();
    return out;  // insufficient information
  }
  out.assign(expectedDecompressedSizeHint, '\0');
  std::size_t const dsz = ZSTD_decompress(out.data(), out.size(), compressed.data(), compressed.size());
  EXPECT_NE(ZSTD_isError(dsz), 1);
  out.resize(dsz);
  return out;
}

}  // namespace aeronet::test
