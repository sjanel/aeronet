#pragma once

#include <brotli/encode.h>

#include <cstddef>
#include <memory>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

class BrotliEncoderContext final : public EncoderContext {
 public:
  BrotliEncoderContext(RawChars &sharedBuf, int quality, int window);

  std::string_view encodeChunk(std::string_view chunk) override;

 private:
  std::unique_ptr<BrotliEncoderState, void (*)(BrotliEncoderState *)> _state;
  RawChars &_buf;
};

class BrotliEncoder {
 public:
  BrotliEncoder() noexcept = default;

  explicit BrotliEncoder(RawChars &buf, const CompressionConfig &cfg)
      : pBuf(&buf), _quality(cfg.brotli.quality), _window(cfg.brotli.window) {}

  std::size_t encodeFull(std::string_view data, std::size_t availableCapacity, char *buf) const;

  std::unique_ptr<EncoderContext> makeContext() {
    return std::make_unique<BrotliEncoderContext>(*pBuf, _quality, _window);
  }

 private:
  RawChars *pBuf{nullptr};
  int _quality{};
  int _window{};
};

}  // namespace aeronet
