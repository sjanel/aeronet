#include "aeronet/raw-bytes.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/internal/raw-bytes-base.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/safe-cast.hpp"

namespace aeronet {

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>::RawBytesBase(uint64_t capacity)
    : _buf(static_cast<value_type*>(std::malloc(SafeCast<size_type>(capacity)))),
      _capacity(static_cast<size_type>(capacity)) {
  if (capacity != 0 && _buf == nullptr) [[unlikely]] {
    throw std::bad_alloc();
  }
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
  if (_capacity != 0) {
    std::memset(_buf, 255, _capacity);
  }
#endif
}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>::RawBytesBase(const RawBytesBase& rhs) : RawBytesBase(rhs.capacity()) {
  _size = rhs.size();
  Copy(rhs.data(), _size, _buf);
}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>::RawBytesBase(RawBytesBase&& rhs) noexcept
    : _buf(std::exchange(rhs._buf, nullptr)),
      _size(std::exchange(rhs._size, static_cast<SizeType>(0))),
      _capacity(std::exchange(rhs._capacity, static_cast<SizeType>(0))) {}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>& RawBytesBase<T, ViewType, SizeType>::operator=(RawBytesBase&& rhs) noexcept {
  if (this != &rhs) [[likely]] {
    std::free(_buf);

    _buf = std::exchange(rhs._buf, nullptr);
    _size = std::exchange(rhs._size, static_cast<SizeType>(0));
    _capacity = std::exchange(rhs._capacity, static_cast<SizeType>(0));
  }
  return *this;
}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>& RawBytesBase<T, ViewType, SizeType>::operator=(const RawBytesBase& rhs) {
  if (this != &rhs) [[likely]] {
    reserve(rhs.size());
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
    if (rhs.size() < _size) {
      std::memset(_buf + rhs.size(), 255, _size - rhs.size());
    }
#endif
    _size = rhs.size();
    Copy(rhs.data(), _size, _buf);
  }
  return *this;
}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>::~RawBytesBase() {
  std::free(_buf);
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
  _buf = nullptr;
#endif
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::assign(const_pointer data, uint64_t size) {
  reserve(size);
  Copy(data, size, _buf);

  _size = static_cast<size_type>(size);
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::erase_front(size_type n) noexcept {
  if (n == _size) {
    _size = 0;
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
    std::memset(_buf, 255, n);
#endif
  } else if (n != 0) {
    assert(n <= _size);
    _size -= n;
    std::memmove(_buf, _buf + n, _size);
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
    std::memset(_buf + _size, 255, n);
#endif
  }
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::reserve(uint64_t newCapacity) {
  if (_capacity < SafeCast<size_type>(newCapacity)) {
    reallocUp(static_cast<size_type>(newCapacity));
  }
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::shrink_to_fit() noexcept {
  static constexpr std::size_t kMinCapacity = 1024;
  if (kMinCapacity < _capacity && 4UL * _size < _capacity) {
    const size_type newCap = _capacity / 2;
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
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
void RawBytesBase<T, ViewType, SizeType>::swap(RawBytesBase& rhs) noexcept {
  using std::swap;

  swap(_buf, rhs._buf);
  swap(_size, rhs._size);
  swap(_capacity, rhs._capacity);
}

template <class T, class ViewType, class SizeType>
bool RawBytesBase<T, ViewType, SizeType>::operator==(const RawBytesBase& rhs) const noexcept {
  return (size() == rhs.size()) && (empty() || std::memcmp(data(), rhs.data(), size()) == 0);
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::reallocUp(size_type newCapacity) {
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
  // force allocation of a new buffer to catch invalid accesses more easily
  pointer newBuf = static_cast<pointer>(std::malloc(newCapacity));
  if (newBuf == nullptr) [[unlikely]] {
    throw std::bad_alloc();
  }
  std::memset(newBuf + _size, 255, newCapacity - _size);
  Copy(_buf, _size, newBuf);
  std::free(_buf);
#else
  pointer newBuf = static_cast<pointer>(std::realloc(_buf, newCapacity));
  if (newBuf == nullptr) [[unlikely]] {
    throw std::bad_alloc();
  }
#endif
  _buf = newBuf;
  _capacity = newCapacity;
}

// Explicit instantiations for commonly used concrete types
template class RawBytesBase<char, std::string_view, std::uint32_t>;
template class RawBytesBase<char, std::string_view, std::uint64_t>;
template class RawBytesBase<std::byte, std::span<const std::byte>, std::uint32_t>;
template class RawBytesBase<std::byte, std::span<const std::byte>, std::uint64_t>;
}  // namespace aeronet
