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

struct ZstdContextRAII {
  ZstdContextRAII(int level, int windowLog);

  std::unique_ptr<ZSTD_CCtx, void (*)(ZSTD_CCtx*)> ctx;
  int level{0};
};

}  // namespace details

class ZstdEncoderContext : public EncoderContext {
 public:
  ZstdEncoderContext(RawChars& sharedBuf, const CompressionConfig::Zstd& cfg)
      : _buf(sharedBuf), _zs(cfg.compressionLevel, cfg.windowLog) {}

  std::string_view encodeChunk(std::size_t encoderChunkSize, std::string_view chunk) override;

 private:
  RawChars& _buf;
  details::ZstdContextRAII _zs;
};

class ZstdEncoder : public Encoder {
 public:
  explicit ZstdEncoder(const CompressionConfig& cfg, std::size_t initialCapacity = 4096UL)
      : _buf(initialCapacity), _cfg(cfg.zstd), _zs(_cfg.compressionLevel, _cfg.windowLog) {}

  void encodeFull(std::size_t extraCapacity, std::string_view data, RawChars& buf) override;

  std::unique_ptr<EncoderContext> makeContext() override { return std::make_unique<ZstdEncoderContext>(_buf, _cfg); }

 private:
  RawChars _buf;
  CompressionConfig::Zstd _cfg;
  details::ZstdContextRAII _zs;
};

}  // namespace aeronet
