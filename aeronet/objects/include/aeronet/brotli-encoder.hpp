#pragma once

#include <brotli/encode.h>

#include <cstddef>
#include <memory>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

class BrotliEncoderContext : public EncoderContext {
 public:
  BrotliEncoderContext(RawChars &sharedBuf, int quality, int window);

  std::string_view encodeChunk(std::size_t encoderChunkSize, std::string_view chunk) override;

 private:
  std::unique_ptr<BrotliEncoderState, void (*)(BrotliEncoderState *)> _state;
  RawChars &_buf;
  bool _finished{false};
};

class BrotliEncoder : public Encoder {
 public:
  explicit BrotliEncoder(const CompressionConfig &cfg, std::size_t initialCapacity = 4096UL)
      : _buf(initialCapacity), _quality(cfg.brotli.quality), _window(cfg.brotli.window) {}

  std::string_view encodeFull(std::size_t encoderChunkSize, std::string_view full) override;

  std::unique_ptr<EncoderContext> makeContext() override {
    return std::make_unique<BrotliEncoderContext>(_buf, _quality, _window);
  }

 private:
  RawChars _buf;
  int _quality;
  int _window;
};

}  // namespace aeronet
