#include "aeronet/brotli-encoder.hpp"

#include <brotli/encode.h>
#include <brotli/types.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
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

int64_t BrotliEncoderContext::encodeChunk(std::string_view data, std::size_t availableCapacity, char* buf) {
  if (data.empty()) {
    return 0;
  }

  const uint8_t* nextIn = reinterpret_cast<const uint8_t*>(data.data());
  std::size_t availIn = data.size();

  uint8_t* nextOut = reinterpret_cast<uint8_t*>(buf);
  std::size_t availOut = availableCapacity;

  if (BrotliEncoderCompressStream(_state.get(), BROTLI_OPERATION_PROCESS, &availIn, &nextIn, &availOut, &nextOut,
                                  nullptr) == BROTLI_FALSE) [[unlikely]] {
    return -1;  // immediate error
  }

  if (availIn != 0) [[unlikely]] {
    // Brotli refused to consume all input → fatal
    return -1;
  }

  return static_cast<int64_t>(availableCapacity - availOut);
}

std::size_t BrotliEncoderContext::maxCompressedBytes(std::size_t uncompressedSize) const {
  return BrotliEncoderMaxCompressedSize(uncompressedSize);
}

int64_t BrotliEncoderContext::end(std::size_t availableCapacity, char* buf) noexcept {
  const uint8_t* nextIn = nullptr;
  std::size_t availIn = 0;

  auto* nextOut = reinterpret_cast<uint8_t*>(buf);
  std::size_t availOut = availableCapacity;
  const std::size_t beforeOut = availOut;

  if (BrotliEncoderCompressStream(_state.get(), BROTLI_OPERATION_FINISH, &availIn, &nextIn, &availOut, &nextOut,
                                  nullptr) == BROTLI_FALSE) [[unlikely]] {
    return -1;
  }

  const std::size_t writtenNow = beforeOut - availOut;
  if (BrotliEncoderIsFinished(_state.get()) == BROTLI_TRUE) {
    return static_cast<int64_t>(writtenNow);
  }

  if (writtenNow == 0) [[unlikely]] {
    return -1;
  }

  return static_cast<int64_t>(writtenNow);
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
                            reinterpret_cast<const uint8_t*>(data.data()), &availableCapacity, dst) == BROTLI_FALSE) {
    availableCapacity = 0U;
  }

  return availableCapacity;
}

}  // namespace aeronet
