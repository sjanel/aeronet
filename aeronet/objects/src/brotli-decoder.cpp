#include "aeronet/brotli-decoder.hpp"

#include <brotli/decode.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace {
struct BrotliRAII {
  explicit BrotliRAII(BrotliDecoderState *state) noexcept : _state(state) {}

  BrotliRAII(const BrotliRAII &) = delete;
  BrotliRAII(BrotliRAII &&) noexcept = delete;
  BrotliRAII &operator=(const BrotliRAII &) = delete;
  BrotliRAII &operator=(BrotliRAII &&) noexcept = delete;

  ~BrotliRAII() { BrotliDecoderDestroyInstance(_state); }

  BrotliDecoderState *_state;
};
}  // namespace

bool BrotliDecoder::Decompress(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                               RawChars &out) {
  BrotliRAII state(BrotliDecoderCreateInstance(nullptr, nullptr, nullptr));
  if (state._state == nullptr) {
    return false;
  }
  const uint8_t *nextIn = reinterpret_cast<const uint8_t *>(input.data());
  std::size_t availIn = input.size();
  while (true) {
    out.ensureAvailableCapacity(decoderChunkSize);
    uint8_t *nextOut = reinterpret_cast<uint8_t *>(out.data() + out.size());
    std::size_t availOut = out.capacity() - out.size();

    auto res = BrotliDecoderDecompressStream(state._state, &availIn, &nextIn, &availOut, &nextOut, nullptr);
    out.setSize(out.capacity() - availOut);

    if (BrotliDecoderHasMoreOutput(state._state) != 0 && availOut == 0) {
      continue;  // need more output space
    }
    if (res == BROTLI_DECODER_RESULT_SUCCESS) {
      return true;
    }
    if (res == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
      if (out.size() > maxDecompressedBytes) {
        return false;
      }
      continue;
    }
    if (res == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
      // Should not happen if we provided entire frame unless truncated
      // treat as corruption
      return false;
    }
    // error
    return false;
  }
}

}  // namespace aeronet
