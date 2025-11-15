#include "aeronet/brotli-encoder.hpp"

#include <brotli/encode.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

BrotliEncoderContext::BrotliEncoderContext(RawChars &sharedBuf, int quality, int window)
    : _state(BrotliEncoderCreateInstance(nullptr, nullptr, nullptr), &BrotliEncoderDestroyInstance), _buf(sharedBuf) {
  if (!_state) {
    throw std::runtime_error("BrotliEncoderCreateInstance failed");
  }
  if (quality >= 0) {
    if (!static_cast<bool>(
            BrotliEncoderSetParameter(_state.get(), BROTLI_PARAM_QUALITY, static_cast<uint32_t>(quality)))) {
      throw std::invalid_argument("Brotli set quality failed");
    }
  }
  if (window > 0) {
    if (!static_cast<bool>(
            BrotliEncoderSetParameter(_state.get(), BROTLI_PARAM_LGWIN, static_cast<uint32_t>(window)))) {
      throw std::invalid_argument("Brotli set window failed");
    }
  }
}

std::string_view BrotliEncoderContext::encodeChunk(std::size_t encoderChunkSize, std::string_view chunk) {
  _buf.clear();
  if (_finished) {
    return _buf;
  }
  const uint8_t *nextIn = reinterpret_cast<const uint8_t *>(chunk.data());
  std::size_t availIn = chunk.size();
  // Shared streaming loop for both one-shot and chunked encoding.
  // Semantics:
  //  - If finish == false: process input until all provided bytes are consumed; do not attempt to finish stream.
  //  - If finish == true: keep invoking the encoder until the stream reports finished (all input consumed and flush
  //  complete).
  for (;;) {
    _buf.ensureAvailableCapacityExponential(encoderChunkSize);

    uint8_t *nextOut = reinterpret_cast<uint8_t *>(_buf.data() + _buf.size());
    std::size_t availOut = _buf.capacity() - _buf.size();

    // Only switch to FINISH operation after all input has been consumed when finish requested.
    BrotliEncoderOperation op = chunk.empty() && availIn == 0 ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS;

    if (BrotliEncoderCompressStream(_state.get(), op, &availIn, &nextIn, &availOut, &nextOut, nullptr) == 0) {
      throw std::runtime_error("BrotliEncoderCompressStream failed");
    }

    _buf.setSize(_buf.capacity() - availOut);

    if (chunk.empty()) {
      // Finishing mode: break only when encoder reports finished after issuing FINISH op
      if (op == BROTLI_OPERATION_FINISH && BrotliEncoderIsFinished(_state.get()) != 0) {
        break;
      }
    } else {
      // Non-finishing mode: stop once caller's input fully consumed OR output buffer filled (loop continues on
      // fill)
      if (availIn == 0) {
        break;
      }
    }

    // If encoder produced output filling current buffer chunk, loop to grow and continue.
    if (availOut == 0) {
      continue;
    }
  }
  if (chunk.empty()) {
    _finished = true;
  }
  return _buf;
}

std::string_view BrotliEncoder::encodeFull(std::size_t encoderChunkSize, std::string_view full) {
  BrotliEncoderContext ctx(_buf, _quality, _window);
  ctx.encodeChunk(encoderChunkSize, full);
  return ctx.encodeChunk(encoderChunkSize, std::string_view{});
}

}  // namespace aeronet
