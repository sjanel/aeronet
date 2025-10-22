#include "zstd_test_helpers.hpp"

#include "exception.hpp"

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>
#endif

#include <cstddef>
#include <string>
#include <string_view>

namespace aeronet::test {

// Decompress a single zstd frame contained in 'compressed'. If the frame size is known
// (via frame header) we trust it; otherwise we fall back to an expected size hint.
// expectedDecompressedSizeHint may be zero; in that case and when the frame size is
// unknown we return an empty string to signal inability (tests can decide how to handle).
std::string zstdRoundTripDecompress([[maybe_unused]] std::string_view compressed,
                                    [[maybe_unused]] std::size_t expectedDecompressedSizeHint) {
#ifdef AERONET_ENABLE_ZSTD
  std::string out;
  if (compressed.empty()) {
    return out;
  }
  const std::size_t frameSize = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
  if (frameSize != ZSTD_CONTENTSIZE_ERROR && frameSize != ZSTD_CONTENTSIZE_UNKNOWN) {
    out.assign(frameSize, '\0');
    const std::size_t dsz = ZSTD_decompress(out.data(), out.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(dsz) == 1) {
      throw exception("ZSTD decompress error");
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
    throw exception("ZSTD decompress error");
  }
  out.resize(dsz);
  return out;
#else
  return {};
#endif
}

}  // namespace aeronet::test
