#include "aeronet/brotli-encoder.hpp"

#include <brotli/encode.h>
#include <brotli/types.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string_view>
#include <utility>

#include "aeronet/encoder-result.hpp"

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
    : _scratch(std::exchange(rhs._scratch, nullptr)), _state(std::move(rhs._state)) {}

BrotliEncoderContext& BrotliEncoderContext::operator=(BrotliEncoderContext&& rhs) noexcept {
  if (this != &rhs) [[likely]] {
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

EncoderResult BrotliEncoderContext::encodeChunk(std::string_view data, std::size_t availableCapacity, char* buf) {
  assert(!data.empty());

  const uint8_t* nextIn = reinterpret_cast<const uint8_t*>(data.data());
  std::size_t availIn = data.size();

  uint8_t* nextOut = reinterpret_cast<uint8_t*>(buf);
  std::size_t availOut = availableCapacity;

  if (BrotliEncoderCompressStream(_state.get(), BROTLI_OPERATION_PROCESS, &availIn, &nextIn, &availOut, &nextOut,
                                  nullptr) == BROTLI_FALSE) [[unlikely]] {
    // TODO: Is there a way to get more information about the failure?
    return EncoderResult(EncoderResult::Error::CompressionError);
  }

  if (availIn != 0) [[unlikely]] {
    // Brotli refused to consume all input → fatal
    // TODO: is this check really necessary?
    return EncoderResult(EncoderResult::Error::CompressionError);
  }

  return EncoderResult(availableCapacity - availOut);
}

std::size_t BrotliEncoderContext::maxCompressedBytes(std::size_t uncompressedSize) const {
  return BrotliEncoderMaxCompressedSize(uncompressedSize);
}

EncoderResult BrotliEncoderContext::end(std::size_t availableCapacity, char* buf) noexcept {
  const uint8_t* nextIn = nullptr;
  std::size_t availIn = 0;

  auto* nextOut = reinterpret_cast<uint8_t*>(buf);
  std::size_t availOut = availableCapacity;

  if (BrotliEncoderCompressStream(_state.get(), BROTLI_OPERATION_FINISH, &availIn, &nextIn, &availOut, &nextOut,
                                  nullptr) == BROTLI_FALSE) [[unlikely]] {
    return EncoderResult(EncoderResult::Error::CompressionError);
  }

  const std::size_t writtenNow = availableCapacity - availOut;
  if (BrotliEncoderIsFinished(_state.get()) == BROTLI_TRUE) {
    return EncoderResult(writtenNow);
  }

  if (writtenNow == 0) [[unlikely]] {
    return EncoderResult(EncoderResult::Error::CompressionError);
  }

  return EncoderResult(writtenNow);
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

EncoderResult BrotliEncoder::encodeFull(std::string_view data, std::size_t availableCapacity, char* buf) {
  _ctx.init(_quality, _window);

  const uint8_t* nextIn = reinterpret_cast<const uint8_t*>(data.data());
  std::size_t availIn = data.size();

  uint8_t* nextOut = reinterpret_cast<uint8_t*>(buf);
  std::size_t availOut = availableCapacity;

  if (BrotliEncoderCompressStream(_ctx._state.get(), BROTLI_OPERATION_FINISH, &availIn, &nextIn, &availOut, &nextOut,
                                  nullptr) == BROTLI_FALSE) [[unlikely]] {
    // In case of not enough capacity, Brotli does not return an error here, we need to check availOut to determine if
    // it was successful or not.
    return EncoderResult(EncoderResult::Error::CompressionError);
  }
  if (BrotliEncoderIsFinished(_ctx._state.get()) == BROTLI_FALSE) [[unlikely]] {
    // If the encoder is not finished, it means it did not fit in the available capacity. Brotli does not return an
    // error in this case, so we need to check this condition explicitly.
    return EncoderResult(EncoderResult::Error::NotEnoughCapacity);
  }

  return EncoderResult(availableCapacity - availOut);
}

}  // namespace aeronet
