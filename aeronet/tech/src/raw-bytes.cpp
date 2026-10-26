#include "aeronet/raw-bytes.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>

#include "aeronet/internal/raw-bytes-base.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/safe-cast.hpp"

namespace aeronet {

void ThrowRawBytesBaseCapacityOverflow(uintmax_t size, uintmax_t extra) {
  log::critical("RawBytesBase: size {} + extra capacity {} overflows uintmax_t", size, extra);
  throw std::overflow_error("required capacity overflows uintmax_t");
}

namespace {

// Buffers growing up to this capacity are copied into a new block instead of being reallocated: realloc can seldom
// extend a small block in place, and it bypasses the fast paths of malloc and free for small blocks (like the thread
// cache of glibc, which holds blocks up to 1032 bytes and takes no lock), which outweigh the copy of the used bytes.
constexpr std::uint64_t kMaxCopiedGrowthCapacity = 1024;

#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
// Growing always into a new buffer catches accesses to the previous one more easily.
constexpr bool kAlwaysGrowIntoNewBuffer = true;
#else
constexpr bool kAlwaysGrowIntoNewBuffer = false;
#endif

}  // namespace

template <class T, class ViewType, class SizeType>
T* RawBytesBase<T, ViewType, SizeType>::Allocate(uint64_t capacity) {
  if (capacity == 0) {
    return nullptr;
  }
  pointer pBuf = static_cast<pointer>(std::malloc(SafeCast<size_type>(capacity)));
  if (pBuf == nullptr) [[unlikely]] {
    throw std::bad_alloc();
  }
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
  std::memset(pBuf, 255, capacity);
#endif
  return pBuf;
}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>::RawBytesBase(const RawBytesBase& rhs) : RawBytesBase(rhs.size()) {
  _size = rhs.size();
  Copy(rhs.data(), _size, _buf);
}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>& RawBytesBase<T, ViewType, SizeType>::operator=(const RawBytesBase& rhs) {
  if (this != &rhs) [[likely]] {
    assign(rhs.data(), rhs.size());
  }
  return *this;
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::assign(const_pointer data, uint64_t size) {
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
  assert(size == 0 || !pointsInto(data));
#endif
  if (_capacity < size) {
    // The previous content is replaced: a new block avoids the copy made by realloc, which copies the whole block when
    // it cannot extend it in place. It is allocated before freeing the previous one, kept if the allocation fails.
    pointer pNewBuf = Allocate(size);
    std::free(_buf);
    _buf = pNewBuf;
    _capacity = static_cast<SizeType>(size);
  }
  Copy(data, size, _buf);

  _size = static_cast<SizeType>(size);
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
  if (_capacity != _size) {
    std::memset(_buf + _size, 255, _capacity - _size);
  }
#endif
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::erasePartialFront(size_type n) noexcept {
  assert(n != 0 && n < _size);
  _size -= n;
  std::memmove(_buf, _buf + n, _size);
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
  std::memset(_buf + _size, 255, n);
#endif
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::shrink_to_fit() noexcept {
  static constexpr std::size_t kMinCapacity = 1024;
  if (kMinCapacity < _capacity && 4UL * _size < _capacity) {
    const size_type newCap = _capacity / 2;
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
    // Using a new buffer to force a fresh allocation and catch invalid accesses more easily.
    pointer newBuf = static_cast<pointer>(std::malloc(newCap));
    if (newBuf != nullptr) [[likely]] {
      Copy(_buf, _size, newBuf);
      std::memset(newBuf + _size, 255, newCap - _size);
      std::free(_buf);
      _buf = newBuf;
      _capacity = newCap;
    }
#else
    pointer newBuf = static_cast<pointer>(std::realloc(_buf, newCap));
    if (newBuf != nullptr) [[likely]] {
      _buf = newBuf;
      _capacity = newCap;
    }
#endif
  }
}

template <class T, class ViewType, class SizeType>
bool RawBytesBase<T, ViewType, SizeType>::operator==(const RawBytesBase& rhs) const noexcept {
  return (size() == rhs.size()) && (empty() || std::memcmp(data(), rhs.data(), size()) == 0);
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::reallocUp(uintmax_t newCapacity) {
  const size_type newCap = SafeCast<size_type>(newCapacity);
  pointer pNewBuf;
  if (kAlwaysGrowIntoNewBuffer || _buf == nullptr || newCap <= kMaxCopiedGrowthCapacity) {
    pNewBuf = static_cast<pointer>(std::malloc(newCap));
    if (pNewBuf == nullptr) [[unlikely]] {
      throw std::bad_alloc();
    }
    // A default constructed buffer has nothing to copy nor free.
    if (_buf != nullptr) {
      Copy(_buf, _size, pNewBuf);
      std::free(_buf);
    }
  } else {
    pNewBuf = static_cast<pointer>(std::realloc(_buf, newCap));
    if (pNewBuf == nullptr) [[unlikely]] {
      throw std::bad_alloc();
    }
  }
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
  std::memset(pNewBuf + _size, 255, newCap - _size);
#endif
  _buf = pNewBuf;
  _capacity = newCap;
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::reallocUpExponential(uintmax_t required) {
  reallocUp(ExponentialCapacity(required, _capacity));
}

// Explicit instantiations for commonly used concrete types
template class RawBytesBase<char, std::string_view, std::uint32_t>;
template class RawBytesBase<char, std::string_view, std::uint64_t>;
template class RawBytesBase<std::byte, std::span<const std::byte>, std::uint32_t>;
template class RawBytesBase<std::byte, std::span<const std::byte>, std::uint64_t>;
}  // namespace aeronet
