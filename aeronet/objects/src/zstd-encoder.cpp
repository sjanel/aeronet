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

ZstdContextRAII::ZstdContextRAII(int level, int windowLog) : ctx(ZSTD_createCCtx()), level(level) {
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

std::string_view ZstdEncoderContext::encodeChunk(std::string_view chunk) {
  ZSTD_inBuffer inBuf{chunk.data(), chunk.size(), 0};
  const auto mode = chunk.empty() ? ZSTD_e_end : ZSTD_e_continue;
  const auto chunkCapacity = ZSTD_CStreamOutSize();

  for (_buf.clear();;) {
    _buf.ensureAvailableCapacityExponential(chunkCapacity);

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

std::size_t ZstdEncoder::encodeFull(std::string_view data, std::size_t availableCapacity, char* buf) const {
  std::size_t written = ZSTD_compress2(_zs.ctx.get(), buf, availableCapacity, data.data(), data.size());
  if (ZSTD_isError(written) != 0U) {
    written = 0;
  }
  return written;
}

}  // namespace aeronet
