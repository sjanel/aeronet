#include "aeronet/zstd-encoder.hpp"

#include <cstddef>
#include <format>
#include <stdexcept>
#include <string_view>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace details {

namespace {
void ZSTD_freeWrapper(ZSTD_CCtx* pCtx) { (void)ZSTD_freeCCtx(pCtx); }
}  // namespace

ZstdContextRAII::ZstdContextRAII(int level, int windowLog) : ctx(ZSTD_createCCtx(), &ZSTD_freeWrapper), level(level) {
  if (!ctx) {
    throw std::runtime_error("ZSTD_createCCtx failed");
  }

  if (ZSTD_isError(ZSTD_CCtx_setParameter(ctx.get(), ZSTD_c_compressionLevel, level)) != 0U) {
    throw std::invalid_argument("ZSTD set level failed");
  }
  if (windowLog > 0) {
    if (ZSTD_isError(ZSTD_CCtx_setParameter(ctx.get(), ZSTD_c_windowLog, windowLog)) != 0U) {
      throw std::invalid_argument("ZSTD set windowLog failed");
    }
  }
}
}  // namespace details

std::string_view ZstdEncoderContext::encodeChunk(std::size_t encoderChunkSize, std::string_view chunk) {
  _buf.clear();
  if (_finished) {
    return _buf;
  }
  ZSTD_outBuffer outBuf{_buf.data(), _buf.capacity(), 0};
  ZSTD_inBuffer inBuf{chunk.data(), chunk.size(), 0};
  const auto mode = chunk.empty() ? ZSTD_e_end : ZSTD_e_continue;
  while (true) {
    _buf.ensureAvailableCapacityExponential(encoderChunkSize);
    outBuf.dst = _buf.data() + outBuf.pos;
    outBuf.size = _buf.capacity() - outBuf.pos;

    std::size_t ret = ZSTD_compressStream2(_zs.ctx.get(), &outBuf, &inBuf, mode);
    if (ZSTD_isError(ret) != 0U) {
      throw std::runtime_error(std::format("ZSTD_compressStream2 error: {}", ZSTD_getErrorName(ret)));
    }
    _buf.setSize(outBuf.pos);
    if (chunk.empty()) {
      if (ret == 0) {
        break;
      }
    } else {
      if (inBuf.pos == inBuf.size) {
        break;
      }
    }
  }
  if (chunk.empty()) {
    _finished = true;
  }
  return _buf;
}

void ZstdEncoder::encodeFull(std::size_t extraCapacity, std::string_view data, RawChars& buf) {
  const auto oldSize = buf.size();
  const auto maxCompressedSize = ZSTD_compressBound(data.size());

  buf.ensureAvailableCapacity(maxCompressedSize + extraCapacity);

  const auto dstCapacity = buf.availableCapacity();

  const auto written = ZSTD_compress2(_zs.ctx.get(), buf.data() + oldSize, dstCapacity, data.data(), data.size());
  if (ZSTD_isError(written) != 0U) {
    throw std::runtime_error(std::format("zstd compress2 error: {}", ZSTD_getErrorName(written)));
  }

  buf.addSize(written);
}

}  // namespace aeronet
