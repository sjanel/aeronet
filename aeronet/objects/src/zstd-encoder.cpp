#include "aeronet/zstd-encoder.hpp"

#include <cassert>
#include <cstddef>
#include <format>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace aeronet {

ZstdEncoderContext::ZstdEncoderContext(ZstdEncoderContext&& rhs) noexcept
    : _pBuf(std::exchange(rhs._pBuf, nullptr)), _ctx(std::move(rhs._ctx)) {}

ZstdEncoderContext& ZstdEncoderContext::operator=(ZstdEncoderContext&& rhs) noexcept {
  if (this != &rhs) [[likely]] {
    _pBuf = std::exchange(rhs._pBuf, nullptr);
    _ctx = std::move(rhs._ctx);
  }
  return *this;
}

void ZstdEncoderContext::init(int level, int windowLog) {
  if (_ctx) {
    ZSTD_CCtx_reset(_ctx.get(), ZSTD_reset_session_and_parameters);
  } else {
    _ctx.reset(ZSTD_createCCtx());
    if (!_ctx) [[unlikely]] {
      throw std::bad_alloc();
    }
  }

  [[maybe_unused]] auto ret = ZSTD_CCtx_setParameter(_ctx.get(), ZSTD_c_compressionLevel, level);
  assert(ZSTD_isError(ret) == 0U);

  if (windowLog > 0) {
    ret = ZSTD_CCtx_setParameter(_ctx.get(), ZSTD_c_windowLog, windowLog);
    assert(ZSTD_isError(ret) == 0U);
  }
}

std::string_view ZstdEncoderContext::encodeChunk(std::string_view chunk) {
  assert(_pBuf != nullptr);
  auto& buf = *_pBuf;

  ZSTD_inBuffer inBuf{chunk.data(), chunk.size(), 0};
  const auto mode = chunk.empty() ? ZSTD_e_end : ZSTD_e_continue;
  const auto chunkCapacity = ZSTD_CStreamOutSize();

  for (buf.clear();;) {
    buf.ensureAvailableCapacityExponential(chunkCapacity);

    // Provide zstd an output window starting at the current end of buf.
    // Important: ZSTD_outBuffer.pos is relative to dst, so always 0 here.
    ZSTD_outBuffer outBuf{buf.data() + buf.size(), buf.availableCapacity(), 0};

    const std::size_t ret = ZSTD_compressStream2(_ctx.get(), &outBuf, &inBuf, mode);
    if (ZSTD_isError(ret) != 0U) [[unlikely]] {
      throw std::runtime_error(std::format("ZSTD_compressStream2 error: {}", ZSTD_getErrorName(ret)));
    }

    buf.addSize(outBuf.pos);
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
  return buf;
}

std::size_t ZstdEncoder::encodeFull(std::string_view data, std::size_t availableCapacity, char* buf) {
  _ctx.init(_cfg.compressionLevel, _cfg.windowLog);
  std::size_t written = ZSTD_compress2(_ctx._ctx.get(), buf, availableCapacity, data.data(), data.size());
  if (ZSTD_isError(written) != 0U) [[unlikely]] {
    written = 0;
  }
  return written;
}

}  // namespace aeronet
