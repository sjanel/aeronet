#include "aeronet/brotli-encoder.hpp"

#include <brotli/encode.h>
#include <brotli/types.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string_view>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

BrotliEncoderContext::BrotliEncoderContext(RawChars &sharedBuf, int quality, int window)
    : _state(BrotliEncoderCreateInstance(nullptr, nullptr, nullptr), &BrotliEncoderDestroyInstance), _buf(sharedBuf) {
  if (!_state) {
    throw std::bad_alloc();
  }
  if (BrotliEncoderSetParameter(_state.get(), BROTLI_PARAM_QUALITY, static_cast<uint32_t>(quality)) == BROTLI_FALSE) {
    throw std::invalid_argument("Brotli set quality failed");
  }
  if (BrotliEncoderSetParameter(_state.get(), BROTLI_PARAM_LGWIN, static_cast<uint32_t>(window)) == BROTLI_FALSE) {
    throw std::invalid_argument("Brotli set window failed");
  }
}

std::string_view BrotliEncoderContext::encodeChunk(std::size_t encoderChunkSize, std::string_view chunk) {
  const uint8_t *nextIn = reinterpret_cast<const uint8_t *>(chunk.data());
  const BrotliEncoderOperation op = chunk.empty() ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS;
  std::size_t availIn = chunk.size();

  // Semantics:
  //  - If finish == false: process input until all provided bytes are consumed; do not attempt to finish stream.
  //  - If finish == true: keep invoking the encoder until the stream reports finished (all input consumed and flush
  //  complete).
  for (_buf.clear();;) {
    _buf.ensureAvailableCapacityExponential(encoderChunkSize);

    uint8_t *nextOut = reinterpret_cast<uint8_t *>(_buf.data() + _buf.size());
    std::size_t availOut = _buf.availableCapacity();

    if (BrotliEncoderCompressStream(_state.get(), op, &availIn, &nextIn, &availOut, &nextOut, nullptr) ==
        BROTLI_FALSE) {
      throw std::runtime_error("BrotliEncoderCompressStream failed");
    }
    _buf.setSize(_buf.capacity() - availOut);

    if (chunk.empty()) {
      // Finishing mode: break only when encoder reports finished after issuing FINISH op
      if (BrotliEncoderIsFinished(_state.get()) == BROTLI_TRUE) {
        break;
      }
    } else {
      // Non-finishing mode: stop once caller's input fully consumed OR output buffer filled (loop continues on fill)
      if (availIn == 0) {
        break;
      }
    }

    // If encoder produced output filling current buffer chunk, loop to grow and continue.
  }
  return _buf;
}

void BrotliEncoder::encodeFull(std::size_t extraCapacity, std::string_view data, RawChars &buf) {
  const auto oldSize = buf.size();
  const std::size_t maxCompressedSize = BrotliEncoderMaxCompressedSize(data.size());

  buf.ensureAvailableCapacity(maxCompressedSize + extraCapacity);

  auto *dst = reinterpret_cast<uint8_t *>(buf.data() + oldSize);
  std::size_t outSize = maxCompressedSize;

  if (BrotliEncoderCompress(_quality, _window, BROTLI_MODE_GENERIC, data.size(),
                            reinterpret_cast<const uint8_t *>(data.data()), &outSize, dst) == BROTLI_FALSE) {
    throw std::runtime_error("BrotliEncoderCompress failed");
  }

  buf.setSize(oldSize + outSize);
}

}  // namespace aeronet
