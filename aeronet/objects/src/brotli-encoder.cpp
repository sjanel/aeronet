#include "brotli-encoder.hpp"

#include <brotli/encode.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "exception.hpp"
#include "raw-chars.hpp"

namespace aeronet {

namespace {
// Shared streaming loop for both one-shot and chunked encoding.
// Semantics:
//  - If finish == false: process input until all provided bytes are consumed; do not attempt to finish stream.
//  - If finish == true: keep invoking the encoder until the stream reports finished (all input consumed and flush
//  complete).
// This mirrors the previous behaviors of encodeChunk (with/without finish) and compressAll (always finish),
// removing code duplication while preserving external API contracts.
inline void brotli_stream_encode(BrotliEncoderState *state, RawChars &buf, const uint8_t *&nextIn, std::size_t &availIn,
                                 bool finish) {
  for (;;) {
    // Ensure we have at least 32 KiB free (RawChars will grow exponentially as needed)
    buf.ensureAvailableCapacity(32UL * 1024);
    uint8_t *nextOut = reinterpret_cast<uint8_t *>(buf.data() + buf.size());
    std::size_t availOut = buf.capacity() - buf.size();

    // Only switch to FINISH operation after all input has been consumed when finish requested.
    BrotliEncoderOperation op;
    if (!finish) {
      op = BROTLI_OPERATION_PROCESS;
    } else {
      op = (availIn == 0) ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS;
    }

    if (BrotliEncoderCompressStream(state, op, &availIn, &nextIn, &availOut, &nextOut, nullptr) == 0) {
      throw exception("BrotliEncoderCompressStream failed");
    }

    // bytes produced = original free space - remaining availOut
    std::size_t produced = (buf.capacity() - buf.size()) - availOut;
    buf.setSize(buf.size() + produced);

    if (!finish) {
      // Non-finishing mode: stop once caller's input fully consumed OR output buffer filled (loop continues on fill)
      if (availIn == 0) {
        break;
      }
    } else {
      // Finishing mode: break only when encoder reports finished after issuing FINISH op
      if (op == BROTLI_OPERATION_FINISH && BrotliEncoderIsFinished(state) != 0) {
        break;
      }
    }

    // If encoder produced output filling current buffer chunk, loop to grow and continue.
    if (availOut == 0) {
      continue;
    }
  }
}
}  // namespace

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

std::string_view BrotliEncoderContext::encodeChunk(std::string_view chunk, bool finish) {
  if (_finished) {
    return {};
  }
  _buf->clear();
  const uint8_t *nextIn = reinterpret_cast<const uint8_t *>(chunk.data());
  std::size_t availIn = chunk.size();
  brotli_stream_encode(_state, *_buf, nextIn, availIn, finish);
  if (finish) {
    _finished = true;
  }
  return *_buf;
}

std::string_view BrotliEncoder::compressAll(std::string_view in) {
  BrotliEncoderContext ctx(_buf, _quality, _window);
  return ctx.encodeChunk(in, true);
}

std::string_view BrotliEncoder::encodeFull(std::string_view full) { return compressAll(full); }

}  // namespace aeronet
