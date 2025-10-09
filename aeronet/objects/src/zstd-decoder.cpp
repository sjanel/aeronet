#include "aeronet/zstd-decoder.hpp"

#include <zstd.h>

#include <cstddef>
#include <string_view>

#include "log.hpp"
#include "raw-chars.hpp"

namespace aeronet {

bool ZstdDecoder::Decompress(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                             RawChars& out) {
  const auto rSize = ZSTD_getFrameContentSize(input.data(), input.size());
  if (rSize == ZSTD_CONTENTSIZE_ERROR) {
    log::error("ZstdDecoder::Decompress - getFrameContentSize returned ZSTD_CONTENTSIZE_ERROR");
    return false;
  }
  if (rSize != ZSTD_CONTENTSIZE_UNKNOWN) {
    if (rSize > maxDecompressedBytes) {
      return false;
    }

    out.ensureAvailableCapacity(rSize);
    const std::size_t result = ZSTD_decompress(out.data() + out.size(), rSize, input.data(), input.size());

    const auto errc = ZSTD_isError(result);
    if (errc != 0U && result != rSize) {
      log::error("ZstdDecoder::Decompress - ZSTD_isError failed with error {} or incorrect size {} != {}", errc, result,
                 rSize);
      return false;
    }

    out.addSize(rSize);
    return true;
  }

  // Unknown size: grow progressively

  struct ZStreamRAII {
    ZStreamRAII() : _stream(ZSTD_createDStream()) {}

    ZStreamRAII(const ZStreamRAII&) = delete;
    ZStreamRAII(ZStreamRAII&&) noexcept = delete;
    ZStreamRAII& operator=(const ZStreamRAII&) = delete;
    ZStreamRAII& operator=(ZStreamRAII&&) noexcept = delete;

    ~ZStreamRAII() { ZSTD_freeDStream(_stream); }

    ZSTD_DStream* _stream;
  };

  ZStreamRAII ss;

  const std::size_t initResult = ZSTD_initDStream(ss._stream);
  if (ZSTD_isError(initResult) != 0U) {
    log::error("ZstdDecoder::Decompress - ZSTD_initDStream failed with error {}", initResult);
    return false;
  }

  ZSTD_inBuffer inBuf{input.data(), input.size(), 0};

  while (inBuf.pos < inBuf.size) {
    out.ensureAvailableCapacity(decoderChunkSize);
    ZSTD_outBuffer output{out.data() + out.size(), out.capacity() - out.size(), 0};
    const std::size_t ret = ZSTD_decompressStream(ss._stream, &output, &inBuf);
    if (ZSTD_isError(ret) != 0U) {
      log::error("ZstdDecoder::Decompress - ZSTD_decompressStream failed with error {}", ret);
      return false;
    }
    out.addSize(output.pos);
  }
  return true;
}

}  // namespace aeronet
