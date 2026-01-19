#include "aeronet/brotli-decoder.hpp"

#include <brotli/decode.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <string_view>

#include "aeronet/decoder-buffer-manager.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

BrotliStreamingContext::BrotliStreamingContext() : _pState(BrotliDecoderCreateInstance(nullptr, nullptr, nullptr)) {
  if (_pState == nullptr) [[unlikely]] {
    throw std::bad_alloc();
  }
}

BrotliStreamingContext::~BrotliStreamingContext() {
  BrotliDecoderDestroyInstance(reinterpret_cast<BrotliDecoderState *>(_pState));
}

bool BrotliStreamingContext::decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                                             std::size_t decoderChunkSize, RawChars &out) {
  if (chunk.empty()) {
    return true;
  }

  const auto *nextIn = reinterpret_cast<const uint8_t *>(chunk.data());
  std::size_t availIn = chunk.size();

  DecoderBufferManager decoderBufferManager(out, decoderChunkSize, maxDecompressedBytes);

  auto *pState = reinterpret_cast<BrotliDecoderState *>(_pState);

  while (true) {
    const bool forceEnd = decoderBufferManager.nextReserve();
    auto *nextOut = reinterpret_cast<uint8_t *>(out.data() + out.size());
    std::size_t availOut = out.availableCapacity();

    const auto res = BrotliDecoderDecompressStream(pState, &availIn, &nextIn, &availOut, &nextOut, nullptr);
    if (res == BROTLI_DECODER_RESULT_ERROR) [[unlikely]] {
      log::error("BrotliDecoderDecompressStream failed with error code {}",
                 static_cast<int>(BrotliDecoderGetErrorCode(pState)));
      return false;
    }
    out.setSize(out.capacity() - availOut);
    if (res == BROTLI_DECODER_RESULT_SUCCESS) {
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

}  // namespace aeronet
