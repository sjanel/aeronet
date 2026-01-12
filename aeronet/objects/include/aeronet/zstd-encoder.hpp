#pragma once

#include <zstd.h>

#include <cstddef>
#include <memory>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace details {

struct ZstdCtxDeleter {
  void operator()(ZSTD_CCtx* ctx) const noexcept { ZSTD_freeCCtx(ctx); }
};

struct ZstdContextRAII {
  ZstdContextRAII() noexcept = default;

  ZstdContextRAII(int level, int windowLog);

  std::unique_ptr<ZSTD_CCtx, ZstdCtxDeleter> ctx;
  int level{0};
};

}  // namespace details

class ZstdEncoderContext final : public EncoderContext {
 public:
  ZstdEncoderContext(RawChars& sharedBuf, const CompressionConfig::Zstd& cfg)
      : _buf(sharedBuf), _zs(cfg.compressionLevel, cfg.windowLog) {}

  std::string_view encodeChunk(std::string_view chunk) override;

 private:
  RawChars& _buf;
  details::ZstdContextRAII _zs;
};

class ZstdEncoder {
 public:
  ZstdEncoder() noexcept = default;

  explicit ZstdEncoder(RawChars& buf, const CompressionConfig& cfg)
      : pBuf(&buf), _cfg(cfg.zstd), _zs(_cfg.compressionLevel, _cfg.windowLog) {}

  std::size_t encodeFull(std::string_view data, std::size_t availableCapacity, char* buf) const;

  std::unique_ptr<EncoderContext> makeContext() { return std::make_unique<ZstdEncoderContext>(*pBuf, _cfg); }

 private:
  RawChars* pBuf{nullptr};
  CompressionConfig::Zstd _cfg;
  details::ZstdContextRAII _zs;
};

}  // namespace aeronet
