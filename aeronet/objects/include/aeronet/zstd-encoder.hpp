#pragma once

#include <zstd.h>

#include <cstddef>
#include <memory>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

class ZstdEncoderContext final : public EncoderContext {
 public:
  ZstdEncoderContext() noexcept = default;

  explicit ZstdEncoderContext(RawChars& sharedBuf) : _pBuf(&sharedBuf) {}

  ZstdEncoderContext(const ZstdEncoderContext&) = delete;
  ZstdEncoderContext(ZstdEncoderContext&& rhs) noexcept;
  ZstdEncoderContext& operator=(const ZstdEncoderContext&) = delete;
  ZstdEncoderContext& operator=(ZstdEncoderContext&& rhs) noexcept;

  ~ZstdEncoderContext() override = default;

  std::string_view encodeChunk(std::string_view chunk) override;

  /// Initialize (or reinitialize) the compression context with given parameters.
  /// Reuses internal allocations if already initialized.
  void init(int level, int windowLog);

 private:
  friend class ZstdEncoder;

  struct ZstdCtxDeleter {
    void operator()(ZSTD_CCtx* ctx) const noexcept { ZSTD_freeCCtx(ctx); }
  };

  RawChars* _pBuf{nullptr};
  std::unique_ptr<ZSTD_CCtx, ZstdCtxDeleter> _ctx;
};

class ZstdEncoder {
 public:
  ZstdEncoder() noexcept = default;

  explicit ZstdEncoder(RawChars& buf, CompressionConfig::Zstd cfg) : _cfg(cfg), _ctx(buf) {}

  std::size_t encodeFull(std::string_view data, std::size_t availableCapacity, char* buf);

  EncoderContext* makeContext() {
    _ctx.init(_cfg.compressionLevel, _cfg.windowLog);
    return &_ctx;
  }

 private:
  CompressionConfig::Zstd _cfg;
  ZstdEncoderContext _ctx;
};

}  // namespace aeronet
