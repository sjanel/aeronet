#include "aeronet/brotli-decoder.hpp"

#include <brotli/decode.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string_view>

#include "aeronet/decoder.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "decoder-helpers.hpp"

namespace aeronet {

namespace {

using BrotliStateUniquePtr = std::unique_ptr<BrotliDecoderState, decltype(&BrotliDecoderDestroyInstance)>;

class BrotliStreamingContext final : public DecoderContext {
 public:
  BrotliStreamingContext() {
    if (_state == nullptr) {
      throw std::bad_alloc();
    }
  }

  bool decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                       std::size_t decoderChunkSize, RawChars &out) override {
    if (chunk.empty()) {
      return true;
    }

    const auto *nextIn = reinterpret_cast<const uint8_t *>(chunk.data());
    std::size_t availIn = chunk.size();

    DecoderBufferManager decoderBufferManager(out, decoderChunkSize, maxDecompressedBytes);

    while (true) {
      const bool forceEnd = decoderBufferManager.nextReserve();
      auto *nextOut = reinterpret_cast<uint8_t *>(out.data() + out.size());
      std::size_t availOut = out.availableCapacity();

      const auto res = BrotliDecoderDecompressStream(_state.get(), &availIn, &nextIn, &availOut, &nextOut, nullptr);
      if (res == BROTLI_DECODER_RESULT_ERROR) [[unlikely]] {
        log::error("BrotliDecoderDecompressStream failed with error code {}",
                   static_cast<int>(BrotliDecoderGetErrorCode(_state.get())));
        return false;
      }
      out.setSize(out.capacity() - availOut);
      if (res == BROTLI_DECODER_RESULT_SUCCESS) {
        _state.reset();
        return availIn == 0;
      }
      if (res == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
        return !finalChunk;
      }
      // res == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT
      if (forceEnd) {
        return false;
      }
    }
  }

 private:
  BrotliStateUniquePtr _state{BrotliDecoderCreateInstance(nullptr, nullptr, nullptr), &BrotliDecoderDestroyInstance};
};
}  // namespace

bool BrotliDecoder::Decompress(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                               RawChars &out) {
  return BrotliDecoder{}.decompressFull(input, maxDecompressedBytes, decoderChunkSize, out);
}

bool BrotliDecoder::decompressFull(std::string_view input, std::size_t maxDecompressedBytes,
                                   std::size_t decoderChunkSize, RawChars &out) {
  return BrotliStreamingContext{}.decompressChunk(input, true, maxDecompressedBytes, decoderChunkSize, out);
}

std::unique_ptr<DecoderContext> BrotliDecoder::makeContext() { return std::make_unique<BrotliStreamingContext>(); }

}  // namespace aeronet
