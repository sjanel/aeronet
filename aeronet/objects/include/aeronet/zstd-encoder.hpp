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
  ZstdEncoderContext(RawChars& sharedBuf, const CompressionConfig::Zstd& cfg);

  std::string_view encodeChunk(std::size_t encoderChunkSize, std::string_view chunk) override;

 private:
  RawChars& _buf;
  bool _finished{false};
  details::ZstdContextRAII _zs;
};

class ZstdEncoder : public Encoder {
 public:
  explicit ZstdEncoder(const CompressionConfig& cfg, std::size_t initialCapacity = 4096UL)
      : _buf(initialCapacity), _cfg(cfg.zstd) {}

  std::string_view encodeFull(std::size_t encoderChunkSize, std::string_view full) override;

  std::unique_ptr<EncoderContext> makeContext() override { return std::make_unique<ZstdEncoderContext>(_buf, _cfg); }

 private:
  RawChars _buf;
  CompressionConfig::Zstd _cfg;
};

}  // namespace aeronet
