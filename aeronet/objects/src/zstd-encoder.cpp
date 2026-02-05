#include "aeronet/zstd-encoder.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string_view>
#include <utility>

namespace aeronet {

ZstdEncoderContext::ZstdEncoderContext(ZstdEncoderContext&& rhs) noexcept
    : _ctx(std::move(rhs._ctx)), _endDone(std::exchange(rhs._endDone, false)) {}

ZstdEncoderContext& ZstdEncoderContext::operator=(ZstdEncoderContext&& rhs) noexcept {
  if (this != &rhs) [[likely]] {
    _ctx = std::move(rhs._ctx);
    _endDone = std::exchange(rhs._endDone, false);
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
  _endDone = false;
}

int64_t ZstdEncoderContext::encodeChunk(std::string_view data, std::size_t availableCapacity, char* buf) {
  if (data.empty()) {
    return 0;
  }

  ZSTD_inBuffer inBuf{data.data(), data.size(), 0};
  ZSTD_outBuffer outBuf{buf, availableCapacity, 0};
  const std::size_t ret = ZSTD_compressStream2(_ctx.get(), &outBuf, &inBuf, ZSTD_e_continue);
  if (ZSTD_isError(ret) != 0U) [[unlikely]] {
    return -1;
  }

  if (inBuf.pos != inBuf.size) [[unlikely]] {
    return -1;
  }

  return static_cast<int64_t>(outBuf.pos);
}

std::size_t ZstdEncoderContext::maxCompressedBytes(std::size_t uncompressedSize) const {
  return std::max(ZSTD_compressBound(uncompressedSize), ZSTD_CStreamOutSize());
}

int64_t ZstdEncoderContext::end(std::size_t availableCapacity, char* buf) noexcept {
  if (_endDone) {
    return 0;
  }

  ZSTD_inBuffer inBuf{nullptr, 0, 0};
  ZSTD_outBuffer outBuf{buf, availableCapacity, 0};
  const std::size_t ret = ZSTD_compressStream2(_ctx.get(), &outBuf, &inBuf, ZSTD_e_end);
  if (ZSTD_isError(ret) != 0U) [[unlikely]] {
    return -1;
  }

  if (ret == 0) {
    _endDone = true;
    return static_cast<int64_t>(outBuf.pos);
  }

  if (outBuf.pos == 0) [[unlikely]] {
    return -1;
  }

  return static_cast<int64_t>(outBuf.pos);
}

std::size_t ZstdEncoder::encodeFull(std::string_view data, std::size_t availableCapacity, char* buf) {
  _ctx.init(_cfg.compressionLevel, _cfg.windowLog);
  std::size_t written = ZSTD_compress2(_ctx._ctx.get(), buf, availableCapacity, data.data(), data.size());
  if (ZSTD_isError(written) != 0U) {
    written = 0;
  }
  return written;
}

}  // namespace aeronet
