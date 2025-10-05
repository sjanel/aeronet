#include "brotli-encoder.hpp"

#include <brotli/encode.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "exception.hpp"
#include "raw-chars.hpp"

namespace aeronet {

BrotliEncoderContext::BrotliEncoderContext(RawChars &sharedBuf, [[maybe_unused]] int quality,
                                           [[maybe_unused]] int window)
    : _buf(&sharedBuf) {
  _state = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
  if (_state == nullptr) {
    throw exception("BrotliEncoderCreateInstance failed");
  }
  if (quality >= 0) {
    BrotliEncoderSetParameter(_state, BROTLI_PARAM_QUALITY, static_cast<uint32_t>(quality));
  }
  if (window > 0) {
    BrotliEncoderSetParameter(_state, BROTLI_PARAM_LGWIN, static_cast<uint32_t>(window));
  }
}

BrotliEncoderContext::BrotliEncoderContext(BrotliEncoderContext &&other) noexcept
    : _state(std::exchange(other._state, nullptr)),
      _buf(std::exchange(other._buf, nullptr)),
      _finished(std::exchange(other._finished, true)) {}

BrotliEncoderContext &BrotliEncoderContext::operator=(BrotliEncoderContext &&other) noexcept {
  if (this != &other) {
    BrotliEncoderDestroyInstance(_state);
    _state = std::exchange(other._state, nullptr);
    _buf = std::exchange(other._buf, nullptr);
    _finished = std::exchange(other._finished, true);
  }
  return *this;
}

BrotliEncoderContext::~BrotliEncoderContext() { BrotliEncoderDestroyInstance(_state); }

std::string_view BrotliEncoderContext::encodeChunk(std::size_t encoderChunkSize, std::string_view chunk, bool finish) {
  if (_finished) {
    return {};
  }
  _buf->clear();
  const uint8_t *nextIn = reinterpret_cast<const uint8_t *>(chunk.data());
  std::size_t availIn = chunk.size();
  // Shared streaming loop for both one-shot and chunked encoding.
  // Semantics:
  //  - If finish == false: process input until all provided bytes are consumed; do not attempt to finish stream.
  //  - If finish == true: keep invoking the encoder until the stream reports finished (all input consumed and flush
  //  complete).
  for (;;) {
    // Ensure we have at least encoderChunkSize free (RawChars will grow exponentially as needed)
    _buf->ensureAvailableCapacity(encoderChunkSize);
    uint8_t *nextOut = reinterpret_cast<uint8_t *>(_buf->data() + _buf->size());
    std::size_t availOut = _buf->capacity() - _buf->size();

    // Only switch to FINISH operation after all input has been consumed when finish requested.
    BrotliEncoderOperation op;
    if (!finish) {
      op = BROTLI_OPERATION_PROCESS;
    } else {
      op = (availIn == 0) ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS;
    }

    if (BrotliEncoderCompressStream(_state, op, &availIn, &nextIn, &availOut, &nextOut, nullptr) == 0) {
      throw exception("BrotliEncoderCompressStream failed");
    }

    // bytes produced = original free space - remaining availOut
    std::size_t produced = (_buf->capacity() - _buf->size()) - availOut;
    _buf->setSize(_buf->size() + produced);

    if (!finish) {
      // Non-finishing mode: stop once caller's input fully consumed OR output buffer filled (loop continues on fill)
      if (availIn == 0) {
        break;
      }
    } else {
      // Finishing mode: break only when encoder reports finished after issuing FINISH op
      if (op == BROTLI_OPERATION_FINISH && BrotliEncoderIsFinished(_state) != 0) {
        break;
      }
    }

    // If encoder produced output filling current buffer chunk, loop to grow and continue.
    if (availOut == 0) {
      continue;
    }
  }
  if (finish) {
    _finished = true;
  }
  return *_buf;
}

std::string_view BrotliEncoder::compressAll(std::size_t encoderChunkSize, std::string_view in) {
  BrotliEncoderContext ctx(_buf, _quality, _window);
  return ctx.encodeChunk(encoderChunkSize, in, true);
}

std::string_view BrotliEncoder::encodeFull(std::size_t encoderChunkSize, std::string_view full) {
  return compressAll(encoderChunkSize, full);
}

}  // namespace aeronet
