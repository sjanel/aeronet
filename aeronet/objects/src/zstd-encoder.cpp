#include "zstd-encoder.hpp"

#include <cstddef>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "exception.hpp"
#include "raw-chars.hpp"

namespace aeronet {

namespace details {
ZstdCStreamRAII::ZstdCStreamRAII(int level, int windowLog) : ctx(ZSTD_createCCtx()), level(level) {
  if (ctx == nullptr) {
    throw exception("ZSTD_createCCtx failed");
  }
  if (ZSTD_isError(ZSTD_CCtx_setParameter(ctx, ZSTD_c_compressionLevel, level)) != 0U) {
    throw exception("ZSTD set level failed");
  }
  if (windowLog > 0) {
    if (ZSTD_isError(ZSTD_CCtx_setParameter(ctx, ZSTD_c_windowLog, windowLog)) != 0U) {
      throw exception("ZSTD set windowLog failed");
    }
  }
}
ZstdCStreamRAII::~ZstdCStreamRAII() { ZSTD_freeCCtx(ctx); }
}  // namespace details

ZstdEncoderContext::ZstdEncoderContext(RawChars& sharedBuf, const CompressionConfig::Zstd& cfg)
    : _buf(sharedBuf), _zs(cfg.compressionLevel, cfg.windowLog) {}

std::string_view ZstdEncoderContext::encodeChunk(std::string_view chunk, bool finish) {
  if (_finished) {
    return {};
  }
  _buf.clear();
  ZSTD_outBuffer outBuf{_buf.data(), _buf.capacity(), 0};
  ZSTD_inBuffer inBuf{chunk.data(), chunk.size(), 0};
  auto mode = finish ? ZSTD_e_end : ZSTD_e_continue;
  while (inBuf.pos < inBuf.size || (finish && mode == ZSTD_e_end)) {
    size_t ret = ZSTD_compressStream2(_zs.ctx, &outBuf, &inBuf, mode);
    if (ZSTD_isError(ret) != 0U) {
      throw exception("ZSTD_compressStream2 error: {}", ZSTD_getErrorName(ret));
    }
    _buf.setSize(outBuf.pos);
    if (outBuf.pos == outBuf.size && (inBuf.pos < inBuf.size || (finish && ret != 0))) {
      _buf.ensureAvailableCapacity(static_cast<std::size_t>(32 * 1024));
      outBuf.dst = _buf.data() + outBuf.pos;
      outBuf.size = _buf.capacity() - outBuf.pos;
      continue;
    }
    if (finish && ret == 0) {
      _finished = true;
      break;
    }
    if (inBuf.pos >= inBuf.size && !finish) {
      break;
    }
  }
  if (finish) {
    _finished = true;
  }
  return std::string_view{_buf.data(), _buf.size()};
}

std::string_view ZstdEncoder::compressAll(std::string_view in) {
  _buf.clear();
  details::ZstdCStreamRAII oneShot(_cfg.compressionLevel, _cfg.windowLog);
  ZSTD_outBuffer outBuf{_buf.data(), _buf.capacity(), 0};
  ZSTD_inBuffer inBuf{in.data(), in.size(), 0};
  while (inBuf.pos < inBuf.size) {
    size_t ret = ZSTD_compressStream2(oneShot.ctx, &outBuf, &inBuf, ZSTD_e_continue);
    if (ZSTD_isError(ret) != 0U) {
      throw exception("zstd compressStream2 error: {}", ZSTD_getErrorName(ret));
    }
    if (outBuf.pos == outBuf.size) {
      _buf.ensureAvailableCapacity(static_cast<std::size_t>(32 * 1024));
      outBuf.dst = _buf.data() + outBuf.pos;
      outBuf.size = _buf.capacity() - outBuf.pos;
    }
  }
  // finalize
  for (;;) {
    size_t ret = ZSTD_compressStream2(oneShot.ctx, &outBuf, &inBuf, ZSTD_e_end);
    if (ZSTD_isError(ret) != 0U) {
      throw exception("zstd finalize error: {}", ZSTD_getErrorName(ret));
    }
    if (ret == 0) {
      break;
    }
    if (outBuf.pos == outBuf.size) {
      _buf.ensureAvailableCapacity(static_cast<std::size_t>(32 * 1024));
      outBuf.dst = _buf.data() + outBuf.pos;
      outBuf.size = _buf.capacity() - outBuf.pos;
    }
  }
  _buf.setSize(outBuf.pos);
  return std::string_view{_buf.data(), _buf.size()};
}

std::string_view ZstdEncoder::encodeFull(std::string_view full) { return compressAll(full); }

}  // namespace aeronet
