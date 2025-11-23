#include "aeronet/brotli-decoder.hpp"

#include <brotli/decode.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>

#include "aeronet/decoder.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace {

using BrotliStateUniquePtr = std::unique_ptr<BrotliDecoderState, decltype(&BrotliDecoderDestroyInstance)>;

class BrotliStreamingContext final : public DecoderContext {
 public:
  BrotliStreamingContext() {
    if (_state == nullptr) {
      throw std::runtime_error("BrotliStreamingContext - BrotliDecoderCreateInstance failed");
    }
  }

  bool decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                       std::size_t decoderChunkSize, RawChars &out) override {
    if (finished()) {
      return chunk.empty();
    }
    if (chunk.empty()) {
      return finalChunk ? finished() : true;
    }

    const auto *nextIn = reinterpret_cast<const uint8_t *>(chunk.data());
    std::size_t availIn = chunk.size();

    while (true) {
      out.ensureAvailableCapacityExponential(decoderChunkSize);
      auto *nextOut = reinterpret_cast<uint8_t *>(out.data() + out.size());
      std::size_t availOut = out.availableCapacity();

      auto res = BrotliDecoderDecompressStream(_state.get(), &availIn, &nextIn, &availOut, &nextOut, nullptr);
      out.setSize(out.capacity() - availOut);
      if (maxDecompressedBytes != 0 && out.size() > maxDecompressedBytes) {
        return false;
      }

      if (res == BROTLI_DECODER_RESULT_SUCCESS) {
        _state.reset();
        return availIn == 0;
      }
      if (res == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
        continue;
      }
      if (res == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
        if (availIn != 0) {
          return false;
        }
        return !finalChunk;
      }
      return false;
    }
  }

 private:
  [[nodiscard]] bool finished() const noexcept { return _state == nullptr; }

  BrotliStateUniquePtr _state{BrotliDecoderCreateInstance(nullptr, nullptr, nullptr), &BrotliDecoderDestroyInstance};
};
}  // namespace

bool BrotliDecoder::Decompress(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                               RawChars &out) {
  BrotliDecoder decoder;
  return decoder.decompressFull(input, maxDecompressedBytes, decoderChunkSize, out);
}

bool BrotliDecoder::decompressFull(std::string_view input, std::size_t maxDecompressedBytes,
                                   std::size_t decoderChunkSize, RawChars &out) {
  BrotliStreamingContext ctx;
  return ctx.decompressChunk(input, true, maxDecompressedBytes, decoderChunkSize, out);
}

std::unique_ptr<DecoderContext> BrotliDecoder::makeContext() { return std::make_unique<BrotliStreamingContext>(); }

}  // namespace aeronet
