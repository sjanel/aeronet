#include "aeronet/zstd-decoder.hpp"

#include <zstd.h>

#include <cstddef>
#include <new>
#include <string_view>

#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "decoder-helpers.hpp"

namespace aeronet {

ZstdStreamingContext::ZstdStreamingContext() : _pState(ZSTD_createDStream()) {
  if (_pState == nullptr) [[unlikely]] {
    throw std::bad_alloc();
  }
  ZSTD_initDStream(reinterpret_cast<ZSTD_DStream *>(_pState));
}

ZstdStreamingContext::~ZstdStreamingContext() { ZSTD_freeDStream(reinterpret_cast<ZSTD_DStream *>(_pState)); }

bool ZstdStreamingContext::decompressChunk(std::string_view chunk, [[maybe_unused]] bool finalChunk,
                                           std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                                           RawChars &out) {
  if (chunk.empty()) {
    return true;
  }
  DecoderBufferManager decoderBufferManager(out, decoderChunkSize, maxDecompressedBytes);

  auto *pState = reinterpret_cast<ZSTD_DStream *>(_pState);

  ZSTD_inBuffer in{chunk.data(), chunk.size(), 0};
  while (in.pos < in.size) {
    const bool forceEnd = decoderBufferManager.nextReserve();
    ZSTD_outBuffer output{out.data() + out.size(), out.availableCapacity(), 0};
    const std::size_t ret = ZSTD_decompressStream(pState, &output, &in);
    if (ZSTD_isError(ret) != 0U) [[unlikely]] {
      log::error("ZSTD_decompressStream failed with error {}", ZSTD_getErrorName(ret));
      return false;
    }
    out.addSize(output.pos);
    if (ret != 0) {
      if (forceEnd) {
        return false;
      }
      continue;
    }

    return in.pos == in.size;
  }
  return true;
}

bool ZstdDecoder::decompressFull(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                                 RawChars &out) {
  const auto rSize = ZSTD_getFrameContentSize(input.data(), input.size());
  switch (rSize) {
    case ZSTD_CONTENTSIZE_ERROR:
      log::error("ZSTD_getFrameContentSize returned ZSTD_CONTENTSIZE_ERROR");
      return false;
    case ZSTD_CONTENTSIZE_UNKNOWN:
      return ZstdStreamingContext{}.decompressChunk(input, true, maxDecompressedBytes, decoderChunkSize, out);
    default: {
      if (maxDecompressedBytes < rSize) {
        return false;
      }

      out.ensureAvailableCapacityExponential(rSize);
      const std::size_t ret = ZSTD_decompress(out.data() + out.size(), rSize, input.data(), input.size());
      if (ZSTD_isError(ret) != 0U) [[unlikely]] {
        log::error("ZSTD_decompress failed with error {}", ZSTD_getErrorName(ret));
        return false;
      }

      out.addSize(rSize);
      return true;
    }
  }
}

}  // namespace aeronet
