#pragma once

#include <brotli/encode.h>

#include <cstddef>
#include <memory>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "encoder.hpp"
#include "raw-chars.hpp"

namespace aeronet {

class BrotliEncoderContext : public EncoderContext {
 public:
  BrotliEncoderContext(RawChars &sharedBuf, int quality, int window);

  BrotliEncoderContext(const BrotliEncoderContext &) = delete;
  BrotliEncoderContext(BrotliEncoderContext &&) noexcept;
  BrotliEncoderContext &operator=(const BrotliEncoderContext &) = delete;
  BrotliEncoderContext &operator=(BrotliEncoderContext &&) noexcept;

  ~BrotliEncoderContext();

  std::string_view encodeChunk(std::size_t encoderChunkSize, std::string_view chunk, bool finish) override;

 private:
  BrotliEncoderState *_state{nullptr};
  RawChars *_buf;
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
  std::string_view compressAll(std::size_t encoderChunkSize, std::string_view in);
  RawChars _buf;
  int _quality;
  int _window;
};

}  // namespace aeronet
