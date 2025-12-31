#include "aeronet/zstd-encoder.hpp"

#include <cassert>
#include <cstddef>
#include <format>
#include <new>
#include <stdexcept>
#include <string_view>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace details {

namespace {
void ZSTD_freeWrapper(ZSTD_CCtx* pCtx) { (void)ZSTD_freeCCtx(pCtx); }
}  // namespace

ZstdContextRAII::ZstdContextRAII(int level, int windowLog) : ctx(ZSTD_createCCtx(), &ZSTD_freeWrapper), level(level) {
  if (!ctx) [[unlikely]] {
    throw std::bad_alloc();
  }

  [[maybe_unused]] auto ret = ZSTD_CCtx_setParameter(ctx.get(), ZSTD_c_compressionLevel, level);
  assert(ZSTD_isError(ret) == 0U);

  if (windowLog > 0) {
    ret = ZSTD_CCtx_setParameter(ctx.get(), ZSTD_c_windowLog, windowLog);
    assert(ZSTD_isError(ret) == 0U);
  }
}
}  // namespace details

std::string_view ZstdEncoderContext::encodeChunk(std::size_t encoderChunkSize, std::string_view chunk) {
  ZSTD_inBuffer inBuf{chunk.data(), chunk.size(), 0};
  const auto mode = chunk.empty() ? ZSTD_e_end : ZSTD_e_continue;
  assert(encoderChunkSize > 0);

  for (_buf.clear();;) {
    _buf.ensureAvailableCapacityExponential(encoderChunkSize);

    // Provide zstd an output window starting at the current end of _buf.
    // Important: ZSTD_outBuffer.pos is relative to dst, so always 0 here.
    ZSTD_outBuffer outBuf{_buf.data() + _buf.size(), _buf.availableCapacity(), 0};

    const std::size_t ret = ZSTD_compressStream2(_zs.ctx.get(), &outBuf, &inBuf, mode);
    if (ZSTD_isError(ret) != 0U) [[unlikely]] {
      throw std::runtime_error(std::format("ZSTD_compressStream2 error: {}", ZSTD_getErrorName(ret)));
    }

    _buf.addSize(outBuf.pos);
    if (chunk.empty()) {
      if (ret == 0) [[likely]] {
        break;
      }
    } else {
      if (inBuf.pos == inBuf.size) {
        break;
      }
    }
  }
  return _buf;
}

void ZstdEncoder::encodeFull(std::size_t extraCapacity, std::string_view data, RawChars& buf) {
  const auto oldSize = buf.size();
  const auto maxCompressedSize = ZSTD_compressBound(data.size());

  buf.ensureAvailableCapacity(maxCompressedSize + extraCapacity);

  const auto dstCapacity = buf.availableCapacity();

  const auto written = ZSTD_compress2(_zs.ctx.get(), buf.data() + oldSize, dstCapacity, data.data(), data.size());
  if (ZSTD_isError(written) != 0U) [[unlikely]] {
    throw std::runtime_error(std::format("zstd compress2 error: {}", ZSTD_getErrorName(written)));
  }

  buf.addSize(written);
}

}  // namespace aeronet
