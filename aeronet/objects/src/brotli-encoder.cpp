#include "aeronet/brotli-encoder.hpp"

#include <brotli/encode.h>
#include <brotli/types.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

#include "aeronet/asan-poison.hpp"
#include "aeronet/encoder-result.hpp"

namespace aeronet {

BrotliScratch::BrotliScratch(BrotliScratch&& rhs) noexcept : _pFirstCached(std::exchange(rhs._pFirstCached, nullptr)) {}

BrotliScratch& BrotliScratch::operator=(BrotliScratch&& rhs) noexcept {
  if (this != &rhs) [[likely]] {
    release();
    _pFirstCached = std::exchange(rhs._pFirstCached, nullptr);
  }
  return *this;
}

void* BrotliScratch::Alloc(void* opaque, std::size_t size) noexcept {
  assert(opaque != nullptr && size <= std::numeric_limits<std::size_t>::max() / 2U);
  auto& self = *static_cast<BrotliScratch*>(opaque);
  // Rounded up so that the lowest bit of the capacity is free for kAllocatedFlag.
  const std::size_t roundedSize = (size + (alignof(Block) - 1U)) & ~(alignof(Block) - 1U);

  // Best fit: the first cached block of at least roundedSize bytes, the list being sorted by capacity.
  Block** ppLink = &self._pFirstCached;
  Block** ppSmallerLink = nullptr;
  while (*ppLink != nullptr && (*ppLink)->capacity() < roundedSize) {
    ppSmallerLink = ppLink;
    ppLink = &(*ppLink)->pNext;
  }

  Block* pBlock = *ppLink;
  if (pBlock != nullptr && pBlock->capacity() / 2U <= roundedSize) {
    *ppLink = pBlock->pNext;
  } else {
    if (ppSmallerLink != nullptr && (*ppSmallerLink)->capacity() >= roundedSize / 2U) {
      // An outgrown copy of a buffer brotli grows (its commands in one-shot mode, by a fraction of the input each
      // time): kept, the copies would add up to several times the memory brotli uses at once.
      Block* pOutgrown = *ppSmallerLink;
      *ppSmallerLink = pOutgrown->pNext;
      std::free(pOutgrown);
    }
    pBlock = static_cast<Block*>(std::malloc(sizeof(Block) + roundedSize));
    if (pBlock == nullptr) [[unlikely]] {
      return nullptr;
    }
    pBlock->capacityAndAllocated = roundedSize;
  }
  pBlock->capacityAndAllocated |= Block::kAllocatedFlag;

  // Only the requested size is addressable: the tail of a larger block stays poisoned.
  AsanPoison(pBlock->data(), pBlock->capacity());
  AsanUnpoison(pBlock->data(), size);
  return pBlock->data();
}

void BrotliScratch::Free(void* opaque, void* address) noexcept {
  assert(opaque != nullptr);
  if (address == nullptr) {
    // Brotli frees null pointers, for buffers it never allocated.
    return;
  }
  auto& self = *static_cast<BrotliScratch*>(opaque);
  Block* pBlock = Block::FromData(address);
  const std::size_t capacity = pBlock->capacity();
  AsanPoison(address, capacity);

  Block** ppLink = &self._pFirstCached;
  while (*ppLink != nullptr && (*ppLink)->capacity() < capacity) {
    ppLink = &(*ppLink)->pNext;
  }
  pBlock->pNext = *ppLink;
  *ppLink = pBlock;
}

void BrotliScratch::trim() noexcept {
  Block** ppLink = &_pFirstCached;
  while (*ppLink != nullptr) {
    Block* pBlock = *ppLink;
    if ((pBlock->capacityAndAllocated & Block::kAllocatedFlag) == 0) {
      *ppLink = pBlock->pNext;
      std::free(pBlock);
    } else {
      pBlock->capacityAndAllocated &= ~Block::kAllocatedFlag;
      ppLink = &pBlock->pNext;
    }
  }
}

void BrotliScratch::release() noexcept {
  while (_pFirstCached != nullptr) {
    std::free(std::exchange(_pFirstCached, _pFirstCached->pNext));
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

  // Brotli has no public reset API, so we recreate the state each time, from the blocks the previous one gave back to
  // the scratch: destroyed first, so that the scratch can trim the blocks it no longer needs and reuse the others.
  _state.reset();
  _scratch->trim();
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

void BrotliEncoderContext::BrotliStateDeleter::operator()(BrotliEncoderState* ptr) const noexcept {
  BrotliEncoderDestroyInstance(ptr);
}

EncoderResult BrotliEncoderContext::encodeChunk(std::string_view& data, std::size_t availableCapacity, char* buf) {
  assert(!data.empty());

  const uint8_t* nextIn = reinterpret_cast<const uint8_t*>(data.data());
  std::size_t availIn = data.size();

  uint8_t* nextOut = reinterpret_cast<uint8_t*>(buf);
  std::size_t availOut = availableCapacity;

  if (BrotliEncoderCompressStream(_state.get(), BROTLI_OPERATION_PROCESS, &availIn, &nextIn, &availOut, &nextOut,
                                  nullptr) == BROTLI_FALSE) [[unlikely]] {
    return EncoderResult(EncoderResult::Error::CompressionError);
  }

  // Brotli stops reading input once an input block is full while the output of the previous ones is still pending, for
  // lack of output room.
  data.remove_prefix(data.size() - availIn);

  return EncoderResult(availableCapacity - availOut);
}

std::size_t BrotliEncoderContext::minEncodeChunkCapacity(std::size_t chunkSize) const {
  // Heuristic: from quality 4, brotli merges several input blocks into one metablock and writes its output at once, so
  // no room bounds the output of one call. A quarter of the input suits compressible data, larger bursts take more
  // calls.
  return BrotliEncoderMaxCompressedSize(chunkSize >> 2U);
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
    // Stream not finished and no output room left for its remaining bytes.
    return EncoderResult(EncoderResult::Error::NotEnoughCapacity);
  }

  return EncoderResult(writtenNow);
}

BrotliEncoder::BrotliEncoder(BrotliEncoder&& rhs) noexcept : _quality(rhs._quality), _window(rhs._window) {
  // The Brotli state is allocated inside rhs._scratch and retains its allocator pointer.
  // Destroy it before moving the pool, then lazily recreate it on the next encode.
  rhs._ctx._state.reset();
  _scratch = std::move(rhs._scratch);
  _ctx._scratch = &_scratch;
  rhs._ctx._scratch = nullptr;
}

BrotliEncoder& BrotliEncoder::operator=(BrotliEncoder&& rhs) noexcept {
  if (this != &rhs) [[likely]] {
    // Both states must be destroyed while their original scratch pools are still alive.
    // Moving _scratch first would free this encoder's pool before Brotli cleanup reads it.
    _ctx._state.reset();
    rhs._ctx._state.reset();

    _quality = rhs._quality;
    _window = rhs._window;
    _scratch = std::move(rhs._scratch);
    _ctx._scratch = &_scratch;
    rhs._ctx._scratch = nullptr;
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
