#include "aeronet/raw-bytes.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/internal/raw-bytes-base.hpp"
#include "aeronet/safe-cast.hpp"

namespace aeronet {

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>::RawBytesBase(uint64_t capacity)
    : _buf(static_cast<value_type *>(std::malloc(SafeCast<size_type>(capacity)))),
      _capacity(static_cast<size_type>(capacity)) {
  if (capacity != 0 && _buf == nullptr) [[unlikely]] {
    throw std::bad_alloc();
  }
}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>::RawBytesBase(ViewType data)
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    : RawBytesBase(data.data(), SafeCast<size_type>(data.size())) {}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>::RawBytesBase(const_pointer data, uint64_t sz) : RawBytesBase(sz) {
  if (sz != 0) {
    std::memcpy(_buf, data, _capacity);
    _size = _capacity;
  }
}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType>::RawBytesBase(const RawBytesBase &rhs) : RawBytesBase(rhs.capacity()) {
  _size = rhs.size();
  if (_size != 0) {
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
  if (this != &rhs) [[likely]] {
    std::free(_buf);

    _buf = std::exchange(rhs._buf, nullptr);
    _size = std::exchange(rhs._size, 0);
    _capacity = std::exchange(rhs._capacity, 0);
  }
  return *this;
}

template <class T, class ViewType, class SizeType>
RawBytesBase<T, ViewType, SizeType> &RawBytesBase<T, ViewType, SizeType>::operator=(const RawBytesBase &rhs) {
  if (this != &rhs) [[likely]] {
    reserve(rhs.size());
    _size = rhs.size();
    if (_size != 0) {
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
void RawBytesBase<T, ViewType, SizeType>::unchecked_append(const_pointer data, uint64_t sz) {
  if (sz != 0) {
    if constexpr (sizeof(size_type) < sizeof(uintmax_t)) {
      if (static_cast<uintmax_t>(std::numeric_limits<size_type>::max()) < sz + _size) [[unlikely]] {
        throw std::overflow_error("capacity overflow");
      }
    }
    std::memcpy(_buf + _size, data, sz);
    _size += static_cast<size_type>(sz);
  }
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::unchecked_append(ViewType data) {
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  unchecked_append(data.data(), SafeCast<size_type>(data.size()));
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::append(const_pointer data, uint64_t sz) {
  ensureAvailableCapacityExponential(sz);
  unchecked_append(data, sz);
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::append(ViewType data) {
  ensureAvailableCapacityExponential(data.size());
  unchecked_append(data.data(), data.size());
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::push_back(value_type byte) {
  ensureAvailableCapacityExponential(1U);
  _buf[_size++] = byte;
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::assign(const_pointer data, uint64_t size) {
  if (size != 0) {
    reserve(size);
    std::memcpy(_buf, data, size);
  }
  _size = static_cast<size_type>(size);
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::assign(ViewType data) {
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  assign(data.data(), SafeCast<size_type>(data.size()));
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::erase_front(size_type n) noexcept {
  if (n != 0) {
    assert(n <= _size);
    _size -= n;
    std::memmove(_buf, _buf + n, _size);
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
  if (_size < _capacity) {
    if (_size == 0) {
      std::free(_buf);
      _buf = nullptr;
      _capacity = 0;
    } else {
      pointer newBuf = static_cast<pointer>(std::realloc(_buf, _size));
      if (newBuf != nullptr) [[likely]] {
        _buf = newBuf;
        _capacity = _size;
      }
    }
  }
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::ensureAvailableCapacity(uint64_t availableCapacity) {
  if constexpr (sizeof(size_type) < sizeof(uintmax_t)) {
    if (static_cast<uintmax_t>(std::numeric_limits<size_type>::max()) <
        static_cast<uintmax_t>(_size) + availableCapacity) [[unlikely]] {
      throw std::overflow_error("capacity overflow");
    }
  }
  reserve(_size + availableCapacity);
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::ensureAvailableCapacityExponential(uint64_t availableCapacity) {
  const uintmax_t required = availableCapacity + _size;

  if (_capacity < required) {
    const uintmax_t doubled = (static_cast<uintmax_t>(_capacity) * 2UL) + 1UL;
    const uintmax_t target = std::max(required, doubled);

    if constexpr (sizeof(size_type) < sizeof(uintmax_t)) {
      static constexpr uintmax_t kMaxCapacity = static_cast<uintmax_t>(std::numeric_limits<size_type>::max());
      if (kMaxCapacity < target) {
        throw std::overflow_error("capacity overflow");
      }
    }

    // Safe to cast now
    reallocUp(static_cast<size_type>(target));
  }
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
  return empty() || std::memcmp(data(), rhs.data(), size()) == 0;
}

template <class T, class ViewType, class SizeType>
void RawBytesBase<T, ViewType, SizeType>::reallocUp(size_type newCapacity) {
  pointer newBuf = static_cast<pointer>(std::realloc(_buf, newCapacity));
  if (newBuf == nullptr) [[unlikely]] {
    throw std::bad_alloc();
  }
  _buf = newBuf;
  _capacity = newCapacity;
}

// Explicit instantiations for commonly used concrete types
template class RawBytesBase<char, std::string_view, std::size_t>;
template class RawBytesBase<char, std::string_view, std::uint32_t>;
template class RawBytesBase<std::byte, std::span<const std::byte>, std::size_t>;
template class RawBytesBase<std::byte, std::span<const std::byte>, std::uint32_t>;
}  // namespace aeronet
