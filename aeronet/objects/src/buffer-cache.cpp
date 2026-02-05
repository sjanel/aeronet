#include "aeronet/buffer-cache.hpp"

#include <cassert>
#include <cstdlib>
#include <utility>

namespace aeronet::internal {

BufferCache::BufferCache(BufferCache&& rhs) noexcept
    : _ownedBuf(std::exchange(rhs._ownedBuf, {})), _givenBuf(std::exchange(rhs._givenBuf, {})) {}

BufferCache& BufferCache::operator=(BufferCache&& rhs) noexcept {
  assert(this != &rhs);  // this internal object cannot be called on itself
  std::free(_ownedBuf.pBuf);
  _ownedBuf = std::exchange(rhs._ownedBuf, {});
  _givenBuf = std::exchange(rhs._givenBuf, {});
  return *this;
}

BufferCache::~BufferCache() { std::free(_ownedBuf.pBuf); }

void* BufferCache::allocate(std::size_t size) noexcept {
  if (_ownedBuf.size < size) {
    void* newBuf = std::realloc(_ownedBuf.pBuf, size);
    if (newBuf == nullptr) {
      return nullptr;
    }
    _ownedBuf = {newBuf, size};
  }

  _givenBuf = std::exchange(_ownedBuf, {});
  return _givenBuf.pBuf;
}

void BufferCache::deallocate(void* ptr) noexcept {
  // Cache this buffer for reuse if we don't have one and we know its size
  if (_ownedBuf.pBuf == nullptr && ptr == _givenBuf.pBuf) {
    // This is a pointer we allocated, we know its size
    _ownedBuf = {ptr, _givenBuf.size};
    _givenBuf = {};
  } else {
    // Either we already have a cached buffer, or we don't recognize this pointer - free it
    std::free(ptr);
  }
}

}  // namespace aeronet::internal
