#include "aeronet/raw-bytes.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>

namespace aeronet {

// Out-of-class definitions for RawBytesBase

namespace {
template <class ToT, class FromT>
constexpr ToT SafeCast(FromT value) {
  if constexpr (sizeof(ToT) < sizeof(FromT)) {
    if (std::cmp_greater(value, std::numeric_limits<ToT>::max())) {
      throw std::length_error("value exceeds target type maximum");
    }
  }
  return static_cast<ToT>(value);
}
}  // namespace

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>::RawBytesBase(size_type capacity)
    : _buf(static_cast<value_type *>(std::malloc(capacity))), _capacity(capacity) {
  if (capacity != 0 && _buf == nullptr) {
    throw std::bad_alloc();
  }
}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>::RawBytesBase(ViewType data) : RawBytesBase(SafeCast<size_type>(data.size())) {
  if (!data.empty()) {
    std::memcpy(_buf, data.data(), _capacity);
    _size = _capacity;
  }
}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>::RawBytesBase(const_pointer first, const_pointer last)
    : RawBytesBase(SafeCast<size_type>(last - first)) {
  assert(first <= last);
  if (first != last) {
    std::memcpy(_buf, first, _capacity);
    _size = _capacity;
  }
}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>::RawBytesBase(const RawBytesBase &rhs) : RawBytesBase(rhs.capacity()) {
  _size = rhs.size();
  if (!empty()) {
    std::memcpy(_buf, rhs.data(), _size);
  }
}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>::RawBytesBase(RawBytesBase &&rhs) noexcept
    : _buf(std::exchange(rhs._buf, nullptr)),
      _size(std::exchange(rhs._size, 0)),
      _capacity(std::exchange(rhs._capacity, 0)) {}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType> &RawBytesBase<T, ViewType, SizeType>::operator=(RawBytesBase &&rhs) noexcept {
  if (this != &rhs) {
    std::free(_buf);
    _buf = std::exchange(rhs._buf, nullptr);
    _size = std::exchange(rhs._size, 0);
    _capacity = std::exchange(rhs._capacity, 0);
  }
  return *this;
}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType> &RawBytesBase<T, ViewType, SizeType>::operator=(const RawBytesBase &rhs) {
  if (this != &rhs) {
    if (capacity() < rhs.capacity()) {
      ensureAvailableCapacity(static_cast<size_type>(rhs.capacity() - capacity()));
    }
    _size = rhs.size();
    if (!empty()) {
      std::memcpy(_buf, rhs.data(), _size);
    }
  }
  return *this;
}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>::~RawBytesBase() {
  std::free(_buf);
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::unchecked_append(const_pointer first, const_pointer last) {
  if (first != last) {
    const std::size_t sz = static_cast<std::size_t>(last - first);
    std::memcpy(_buf + _size, first, sz);
    _size += static_cast<size_type>(sz);
  }
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::append(const_pointer first, const_pointer last) {
  assert(first <= last);
  ensureAvailableCapacity(SafeCast<size_type>(last - first));
  unchecked_append(first, last);
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::push_back(value_type byte) {
  ensureAvailableCapacity(1U);
  _buf[_size++] = byte;
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::assign(const_pointer data, size_type size) {
  reserveExponential(size);
  if (size != 0) {
    std::memcpy(_buf, data, size);
  }
  _size = size;
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::assign(ViewType data) {
  assign(data.data(), SafeCast<size_type>(data.size()));
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::assign(const_pointer first, const_pointer last) {
  assign(first, SafeCast<size_type>(last - first));
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::erase_front(size_type n) {
  assert(n <= _size);
  if (n != 0) {
    std::memmove(_buf, _buf + n, _size - n);
    _size -= n;
  }
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::setSize(size_type newSize) {
  assert(newSize <= _capacity);
  _size = newSize;
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::addSize(size_type delta) {
  assert(_size + delta <= _capacity);
  _size += delta;
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::reserveExponential(size_type newCapacity) {
  if (_capacity < newCapacity) {
    if constexpr (sizeof(uintmax_t) > sizeof(size_type)) {
      // prevent overflow when doubling capacity
      static constexpr uintmax_t kMaxCapacity = static_cast<uintmax_t>(std::numeric_limits<size_type>::max());
      if ((static_cast<uintmax_t>(_capacity) * 2UL) + 1UL > kMaxCapacity) {
        throw std::bad_alloc();
      }
    }
    const auto doubledCapacity = static_cast<size_type>((_capacity * size_type{2}) + size_type{1});
    // NOLINTNEXTLINE(readability-use-std-min-max) to avoid include of <algorithm> which is a big include
    if (newCapacity < doubledCapacity) {
      newCapacity = doubledCapacity;
    }
    reallocUp(newCapacity);
  }
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::reserve(size_type newCapacity) {
  if (_capacity < newCapacity) {
    reallocUp(newCapacity);
  }
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::ensureAvailableCapacity(size_type availableCapacity) {
  if constexpr (sizeof(size_type) < sizeof(uintmax_t)) {
    static constexpr uintmax_t kMaxCapacity = static_cast<uintmax_t>(std::numeric_limits<size_type>::max());
    if (kMaxCapacity < static_cast<uintmax_t>(_size) + availableCapacity) {
      throw std::bad_alloc();
    }
  }
  reserveExponential(_size + availableCapacity);
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::swap(RawBytesBase &rhs) noexcept {
  using std::swap;
  swap(_buf, rhs._buf);
  swap(_size, rhs._size);
  swap(_capacity, rhs._capacity);
}

template <class T, class ViewType, class SizeType>
bool RawBytesBase<T, ViewType, SizeType>::operator==(const RawBytesBase &rhs) const noexcept {
  if (size() != rhs.size()) {
    return false;
  }
  const auto *lhsData = data();
  const auto *rhsData = rhs.data();
  if (lhsData != nullptr && rhsData != nullptr) {
    // This is because calling memcmp with nullptr is undefined behavior even if size is zero
    return std::memcmp(lhsData, rhsData, size()) == 0;
  }
  // if we are here, size is same and at least one is nullptr, so both size must be zero
  return true;
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::reallocUp(size_type newCapacity) {
  pointer newBuf = static_cast<pointer>(std::realloc(_buf, newCapacity));
  if (newBuf == nullptr) {
    throw std::bad_alloc();
  }
  _buf = newBuf;
  _capacity = newCapacity;
}

// Explicit instantiations for commonly used concrete types
template class RawBytesBase<char, std::string_view, std::size_t>;
template class RawBytesBase<char, std::string_view, std::uint32_t>;
template class RawBytesBase<char, std::string_view, std::uint8_t>;
template class RawBytesBase<std::byte, std::span<const std::byte>, std::size_t>;
}  // namespace aeronet
