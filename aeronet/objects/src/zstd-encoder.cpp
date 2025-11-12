#include "aeronet/zstd-encoder.hpp"

#include <cstddef>
#include <format>
#include <stdexcept>
#include <string_view>

#include "aeronet/compression-config.hpp"
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

ZstdEncoderContext::ZstdEncoderContext(RawChars& sharedBuf, const CompressionConfig::Zstd& cfg)
    : _buf(sharedBuf), _zs(cfg.compressionLevel, cfg.windowLog) {}

std::string_view ZstdEncoderContext::encodeChunk(std::size_t encoderChunkSize, std::string_view chunk) {
  _buf.clear();
  if (_finished) {
    return _buf;
  }
  ZSTD_outBuffer outBuf{_buf.data(), _buf.capacity(), 0};
  ZSTD_inBuffer inBuf{chunk.data(), chunk.size(), 0};
  const auto mode = chunk.empty() ? ZSTD_e_end : ZSTD_e_continue;
  while (true) {
    _buf.ensureAvailableCapacity(encoderChunkSize);
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

std::string_view ZstdEncoder::encodeFull(std::size_t encoderChunkSize, std::string_view full) {
  _buf.clear();
  details::ZstdContextRAII ctxRAII(_cfg.compressionLevel, _cfg.windowLog);
  ZSTD_outBuffer outBuf{_buf.data(), _buf.capacity(), 0};
  ZSTD_inBuffer inBuf{full.data(), full.size(), 0};

  ZSTD_EndDirective op = ZSTD_e_continue;

  while (true) {
    std::size_t ret = ZSTD_compressStream2(ctxRAII.ctx.get(), &outBuf, &inBuf, op);

    if (ZSTD_isError(ret) != 0U) {
      throw std::runtime_error(std::format("zstd compressStream2 error: {}", ZSTD_getErrorName(ret)));
    }
    if (op == ZSTD_e_end && ret == 0) {
      break;
    }
    if (outBuf.pos == outBuf.size) {
      _buf.ensureAvailableCapacity(encoderChunkSize);
      outBuf.dst = _buf.data() + outBuf.pos;
      outBuf.size = _buf.capacity() - outBuf.pos;
    }
    if (op == ZSTD_e_continue && inBuf.pos == inBuf.size) {
      op = ZSTD_e_end;
    }
  }

  _buf.setSize(outBuf.pos);
  return _buf;
}

}  // namespace aeronet
