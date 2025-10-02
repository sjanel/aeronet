#pragma once

#include <gtest/gtest.h>

#include <cassert>
#include <string>
#include <string_view>

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>
#endif

namespace aeronet::test {

// Decompress a single zstd frame contained in 'compressed'. If the frame size is known
// (via frame header) we trust it; otherwise we fall back to an expected size hint.
// expectedDecompressedSizeHint may be zero; in that case and when the frame size is
// unknown we return an empty string to signal inability (tests can decide how to handle).
inline std::string zstdRoundTripDecompress([[maybe_unused]] std::string_view compressed,
                                           [[maybe_unused]] std::size_t expectedDecompressedSizeHint = 0) {
#ifndef AERONET_ENABLE_ZSTD
  return {};  // zstd not enabled
#else
  if (compressed.empty()) {
    return {};
  }
  size_t frameSize = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
  std::string out;
  if (frameSize != ZSTD_CONTENTSIZE_ERROR && frameSize != ZSTD_CONTENTSIZE_UNKNOWN) {
    out.assign(frameSize, '\0');
    size_t const dsz = ZSTD_decompress(out.data(), out.size(), compressed.data(), compressed.size());
    EXPECT_NE(ZSTD_isError(dsz), 1);
    out.resize(dsz);
    return out;
  }
  if (expectedDecompressedSizeHint == 0) {
    return {};  // insufficient information
  }
  out.assign(expectedDecompressedSizeHint, '\0');
  size_t const dsz = ZSTD_decompress(out.data(), out.size(), compressed.data(), compressed.size());
  EXPECT_NE(ZSTD_isError(dsz), 1);
  out.resize(dsz);
  return out;
#endif
}

}  // namespace aeronet::test
