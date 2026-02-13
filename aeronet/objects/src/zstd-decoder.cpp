#include "aeronet/zstd-decoder.hpp"

#include <zstd.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/decoder-buffer-manager.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

ZstdDecoderContext::ZstdDecoderContext(ZstdDecoderContext&& rhs) noexcept
    : _pState(std::exchange(rhs._pState, nullptr)) {}

ZstdDecoderContext& ZstdDecoderContext::operator=(ZstdDecoderContext&& rhs) noexcept {
  if (this != &rhs) [[likely]] {
    ZSTD_freeDStream(reinterpret_cast<ZSTD_DStream*>(_pState));
    _pState = std::exchange(rhs._pState, nullptr);
  }
  return *this;
}

ZstdDecoderContext::~ZstdDecoderContext() { ZSTD_freeDStream(reinterpret_cast<ZSTD_DStream*>(_pState)); }

void ZstdDecoderContext::init() {
  if (_pState == nullptr) {
    _pState = ZSTD_createDStream();
    if (_pState == nullptr) [[unlikely]] {
      throw std::bad_alloc();
    }
    ZSTD_initDStream(reinterpret_cast<ZSTD_DStream*>(_pState));
  } else {  // Use ZSTD_DCtx_reset instead of free/create to reuse internal buffers
    const auto ret = ZSTD_DCtx_reset(reinterpret_cast<ZSTD_DStream*>(_pState), ZSTD_reset_session_only);
    if (ZSTD_isError(ret) != 0U) [[unlikely]] {
      throw std::runtime_error("Failed to reset Zstd decoder context");
    }
  }
}

bool ZstdDecoderContext::decompressChunk(std::string_view chunk, [[maybe_unused]] bool finalChunk,
                                         std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                                         RawChars& out) {
  if (chunk.empty()) {
    return true;
  }
  DecoderBufferManager decoderBufferManager(out, decoderChunkSize, maxDecompressedBytes);

  auto* pState = reinterpret_cast<ZSTD_DStream*>(_pState);

  ZSTD_inBuffer in{chunk.data(), chunk.size(), 0};
  while (in.pos < in.size) {
    const bool forceEnd = decoderBufferManager.nextReserve();
    ZSTD_outBuffer output{out.data() + out.size(), out.availableCapacity(), 0};
    const std::size_t ret = ZSTD_decompressStream(pState, &output, &in);
    if (ZSTD_isError(ret) != 0U) [[unlikely]] {
      log::debug("ZSTD_decompressStream failed with error {}", ZSTD_getErrorName(ret));
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
                                 RawChars& out) {
  const auto rSize = ZSTD_getFrameContentSize(input.data(), input.size());
  switch (rSize) {
    case ZSTD_CONTENTSIZE_ERROR:
      log::debug("ZSTD_getFrameContentSize returned ZSTD_CONTENTSIZE_ERROR");
      return false;
    case ZSTD_CONTENTSIZE_UNKNOWN:
      _ctx.init();
      return _ctx.decompressChunk(input, true, maxDecompressedBytes, decoderChunkSize, out);
    default: {
      if (maxDecompressedBytes < rSize) {
        return false;
      }

      out.ensureAvailableCapacityExponential(static_cast<uint64_t>(rSize));
      const std::size_t ret = ZSTD_decompress(out.data() + out.size(), rSize, input.data(), input.size());
      if (ZSTD_isError(ret) != 0U) [[unlikely]] {
        log::debug("ZSTD_decompress failed with error {}", ZSTD_getErrorName(ret));
        return false;
      }

      out.addSize(rSize);
      return true;
    }
  }
}

}  // namespace aeronet
