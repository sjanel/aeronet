#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "aeronet/buffer-cache.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/encoder-result.hpp"
#include "aeronet/encoder.hpp"

extern "C" struct ZSTD_CCtx_s;
using ZSTD_CCtx = ZSTD_CCtx_s;

namespace aeronet {

class ZstdEncoderContext final : public EncoderContext {
 public:
  ZstdEncoderContext() noexcept = default;

  ZstdEncoderContext(const ZstdEncoderContext&) = delete;
  ZstdEncoderContext(ZstdEncoderContext&& rhs) noexcept;
  ZstdEncoderContext& operator=(const ZstdEncoderContext&) = delete;
  ZstdEncoderContext& operator=(ZstdEncoderContext&& rhs) noexcept;

  ~ZstdEncoderContext() override = default;

  [[nodiscard]] std::size_t minEncodeChunkCapacity(std::size_t chunkSize) const override;

  [[nodiscard]] std::size_t endChunkSize() const override;

  EncoderResult encodeChunk(std::string_view data, std::size_t availableCapacity, char* buf) override;

  EncoderResult end(std::size_t availableCapacity, char* buf) noexcept override;

  /// Initialize (or reinitialize) the compression context with given parameters.
  /// Reuses internal allocations if already initialized.
  void init(int level, int windowLog);

 private:
  friend class ZstdEncoder;

  struct ZstdCtxDeleter {
    void operator()(ZSTD_CCtx* ctx) const noexcept;
  };

  internal::BufferCache _cache;
  std::unique_ptr<ZSTD_CCtx, ZstdCtxDeleter> _ctx;
  bool _endDone{false};
};

class ZstdEncoder {
 public:
  ZstdEncoder() noexcept = default;

  explicit ZstdEncoder(CompressionConfig::Zstd cfg) noexcept : _cfg(cfg) {}

  EncoderResult encodeFull(std::string_view data, std::size_t availableCapacity, char* buf);

  EncoderContext* makeContext() {
    _ctx.init(_cfg.compressionLevel, _cfg.windowLog);
    return &_ctx;
  }

  EncoderContext* context() { return &_ctx; }

 private:
  CompressionConfig::Zstd _cfg;
  ZstdEncoderContext _ctx;
};

}  // namespace aeronet
