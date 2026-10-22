#include "aeronet/zstd-decoder.hpp"

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/buffer-cache.hpp"
#include "aeronet/decoder-buffer-manager.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace {

void* ZstdDecoderAlloc(void* opaque, std::size_t size) {
  return static_cast<internal::BufferCache*>(opaque)->allocate(size);
}

void ZstdDecoderFree(void* opaque, void* address) noexcept {
  static_cast<internal::BufferCache*>(opaque)->deallocate(address);
}

}  // namespace

ZstdDecoderContext::ZstdDecoderContext(ZstdDecoderContext&& rhs) noexcept : _cache(std::move(rhs._cache)) {
  // Don't transfer rhs state - it holds a stale opaque pointer to rhs._cache.
  // _pState stays nullptr; init() will create a fresh context with &_cache.
  ZSTD_freeDStream(reinterpret_cast<ZSTD_DStream*>(rhs._pState));
  rhs._pState = nullptr;
}

ZstdDecoderContext& ZstdDecoderContext::operator=(ZstdDecoderContext&& rhs) noexcept {
  if (this != &rhs) [[likely]] {
    ZSTD_freeDStream(reinterpret_cast<ZSTD_DStream*>(_pState));
    _pState = nullptr;
    _cache = std::move(rhs._cache);
    ZSTD_freeDStream(reinterpret_cast<ZSTD_DStream*>(rhs._pState));
    rhs._pState = nullptr;
  }
  return *this;
}

ZstdDecoderContext::~ZstdDecoderContext() { ZSTD_freeDStream(reinterpret_cast<ZSTD_DStream*>(_pState)); }

void ZstdDecoderContext::init() {
  _frameComplete = false;
  if (_pState == nullptr) {
    _pState = ZSTD_createDCtx_advanced(ZSTD_customMem{ZstdDecoderAlloc, ZstdDecoderFree, &_cache});
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

bool ZstdDecoderContext::decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                                         uint32_t decoderChunkSize, RawChars& out) {
  if (chunk.empty()) {
    return !finalChunk || _frameComplete;
  }
  DecoderBufferManager decoderBufferManager(out, decoderChunkSize, maxDecompressedBytes);

  auto* pState = reinterpret_cast<ZSTD_DStream*>(_pState);

  ZSTD_inBuffer in{chunk.data(), chunk.size(), 0};
  for (;;) {
    const std::size_t room = decoderBufferManager.nextReserve();  // before out.data(), which it may reallocate
    ZSTD_outBuffer output{out.data() + out.size(), room, 0};
    const std::size_t ret = ZSTD_decompressStream(pState, &output, &in);
    if (ZSTD_isError(ret) != 0U) [[unlikely]] {
      log::debug("ZSTD_decompressStream failed with error {}", ZSTD_getErrorName(ret));
      return false;
    }
    out.addSize(output.pos);
    if (decoderBufferManager.limitExceeded()) {
      log::debug("ZstdDecoderContext::decompressChunk - exceeded max decompressed size of {}", maxDecompressedBytes);
      return false;
    }
    if (ret == 0 && in.pos == in.size) {
      _frameComplete = true;
      return true;  // the input ends with a fully decoded and flushed frame
    }
    // Otherwise a frame is in progress, or another one follows (content may have several frames, RFC 8878 §3.1): the
    // next call to ZSTD_decompressStream() starts it.
    if (in.pos == in.size && output.pos < output.size) {
      // All the input is consumed and nothing is left to flush: the frame continues in the next chunk, so it is
      // truncated when this one is the last. A full output buffer may instead leave decoded bytes in the decoder (see
      // ZSTD_decompressStream()), flushed by the next iteration.
      _frameComplete = false;
      return !finalChunk;
    }
  }
}

bool ZstdDecoder::DeclaresMoreThan(std::string_view input, std::size_t maxDecompressedBytes) noexcept {
  const auto declaredSize = ZSTD_getFrameContentSize(input.data(), input.size());
  return declaredSize != ZSTD_CONTENTSIZE_ERROR && declaredSize != ZSTD_CONTENTSIZE_UNKNOWN &&
         maxDecompressedBytes < declaredSize;
}

bool ZstdDecoder::decompressFull(std::string_view input, std::size_t maxDecompressedBytes, uint32_t decoderChunkSize,
                                 RawChars& out) {
  const auto decompressedSize = ZSTD_getFrameContentSize(input.data(), input.size());
  switch (decompressedSize) {
    case ZSTD_CONTENTSIZE_ERROR:
      log::debug("ZSTD_getFrameContentSize returned ZSTD_CONTENTSIZE_ERROR");
      return false;
    case ZSTD_CONTENTSIZE_UNKNOWN:
      _ctx.init();
      return _ctx.decompressChunk(input, true, maxDecompressedBytes, decoderChunkSize, out);
    default: {
      if (maxDecompressedBytes < decompressedSize) {
        return false;
      }

      // The size declared by the first frame only sizes the output: later frames may follow.
      out.ensureAvailableCapacityExponential(static_cast<uint64_t>(decompressedSize));

      _ctx.init();
      return _ctx.decompressChunk(input, true, maxDecompressedBytes, decoderChunkSize, out);
    }
  }
}

}  // namespace aeronet
