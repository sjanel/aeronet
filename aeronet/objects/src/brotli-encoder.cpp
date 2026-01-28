#include "aeronet/brotli-encoder.hpp"

#include <brotli/encode.h>
#include <brotli/types.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace aeronet {

void* BrotliScratch::Alloc(void* opaque, size_t size) {
  assert(opaque != nullptr);
  try {
    return static_cast<BrotliScratch*>(opaque)->alloc(size);
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

BrotliEncoderContext::BrotliEncoderContext(BrotliEncoderContext&& rhs) noexcept
    : _pBuf(std::exchange(rhs._pBuf, nullptr)),
      _scratch(std::exchange(rhs._scratch, nullptr)),
      _state(std::move(rhs._state)) {}

BrotliEncoderContext& BrotliEncoderContext::operator=(BrotliEncoderContext&& rhs) noexcept {
  if (this != &rhs) [[likely]] {
    _pBuf = std::exchange(rhs._pBuf, nullptr);
    _scratch = std::exchange(rhs._scratch, nullptr);
    _state = std::move(rhs._state);
  }
  return *this;
}

void BrotliEncoderContext::init(int quality, int window) {
  assert(_scratch != nullptr);

  // Brotli has no public reset API, so we recreate the state each time while reusing our scratch allocator.
  _scratch->clear();
  _state.reset(BrotliEncoderCreateInstance(&BrotliScratch::Alloc, &BrotliScratch::Free, _scratch));
  if (!_state) [[unlikely]] {
    throw std::bad_alloc();
  }

  [[maybe_unused]] auto res =
      BrotliEncoderSetParameter(_state.get(), BROTLI_PARAM_QUALITY, static_cast<uint32_t>(quality));
  assert(res == BROTLI_TRUE);
  res = BrotliEncoderSetParameter(_state.get(), BROTLI_PARAM_LGWIN, static_cast<uint32_t>(window));
  assert(res == BROTLI_TRUE);
}

std::string_view BrotliEncoderContext::encodeChunk(std::string_view chunk) {
  const uint8_t* nextIn = reinterpret_cast<const uint8_t*>(chunk.data());
  const BrotliEncoderOperation op = chunk.empty() ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS;
  const auto chunkCapacity = BrotliEncoderMaxCompressedSize(chunk.size());
  std::size_t availIn = chunk.size();

  assert(_pBuf != nullptr);
  auto& buf = *_pBuf;

  // Semantics:
  //  - If finish == false: process input until all provided bytes are consumed; do not attempt to finish stream.
  //  - If finish == true: keep invoking the encoder until the stream reports finished (all input consumed and flush
  //  complete).
  for (buf.clear();;) {
    buf.ensureAvailableCapacityExponential(chunkCapacity);

    uint8_t* nextOut = reinterpret_cast<uint8_t*>(buf.data() + buf.size());
    std::size_t availOut = buf.availableCapacity();

    if (BrotliEncoderCompressStream(_state.get(), op, &availIn, &nextIn, &availOut, &nextOut, nullptr) == BROTLI_FALSE)
        [[unlikely]] {
      throw std::runtime_error("BrotliEncoderCompressStream failed");
    }
    buf.setSize(buf.capacity() - availOut);

    if (chunk.empty()) {
      // Finishing mode: break only when encoder reports finished after issuing FINISH op
      if (BrotliEncoderIsFinished(_state.get()) == BROTLI_TRUE) [[likely]] {
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
  return buf;
}

BrotliEncoder::BrotliEncoder(BrotliEncoder&& rhs) noexcept
    : _quality(rhs._quality), _window(rhs._window), _scratch(std::move(rhs._scratch)), _ctx(std::move(rhs._ctx)) {
  _ctx._scratch = &_scratch;
  _ctx._state.reset();
}

BrotliEncoder& BrotliEncoder::operator=(BrotliEncoder&& rhs) noexcept {
  if (this != &rhs) [[likely]] {
    _quality = rhs._quality;
    _window = rhs._window;
    _ctx = std::move(rhs._ctx);
    _scratch = std::move(rhs._scratch);
    _ctx._scratch = &_scratch;
    _ctx._state.reset();
  }
  return *this;
}

std::size_t BrotliEncoder::encodeFull(std::string_view data, std::size_t availableCapacity, char* buf) const {
  uint8_t* dst = reinterpret_cast<uint8_t*>(buf);
  if (BrotliEncoderCompress(_quality, _window, BROTLI_MODE_GENERIC, data.size(),
                            reinterpret_cast<const uint8_t*>(data.data()), &availableCapacity, dst) == BROTLI_FALSE)
      [[unlikely]] {
    availableCapacity = 0U;
  }

  return availableCapacity;
}

}  // namespace aeronet
