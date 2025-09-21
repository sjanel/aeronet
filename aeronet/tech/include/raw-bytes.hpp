#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <istream>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace aeronet {

/**
 * A simple buffer class that manages a dynamically allocated buffer.
 * It is designed to be used by compression libraries (gzip, zstd) that
 * require a simple, low-level buffer interface, do not use it for general-purpose data storage (prefer vector in that
 * case).
 */
template <class T = std::byte, class ViewType = std::span<const std::byte>>
class RawBytes {
 public:
  using value_type = T;
  using size_type = std::size_t;
  using pointer = value_type *;
  using const_pointer = const value_type *;
  using reference = value_type &;
  using const_reference = const value_type &;
  using iterator = value_type *;
  using const_iterator = const value_type *;

  static_assert(std::is_trivially_copyable_v<T> && sizeof(T) == 1);

  RawBytes() noexcept = default;

  explicit RawBytes(size_type capacity) : _buf(static_cast<value_type *>(std::malloc(capacity))), _capacity(capacity) {
    if (capacity != 0 && _buf == nullptr) {
      throw std::bad_alloc();
    }
  }

  RawBytes(const_pointer first, const_pointer last) : RawBytes(last - first) {
    std::memcpy(_buf, first, _capacity);
    _size = _capacity;
  }

  RawBytes(const RawBytes &) = delete;

  RawBytes(RawBytes &&rhs) noexcept
      : _buf(std::exchange(rhs._buf, nullptr)),
        _size(std::exchange(rhs._size, 0)),
        _capacity(std::exchange(rhs._capacity, 0)) {}

  RawBytes &operator=(RawBytes &&rhs) noexcept {
    if (this != &rhs) {
      std::free(_buf);
      _buf = std::exchange(rhs._buf, nullptr);
      _size = std::exchange(rhs._size, 0);
      _capacity = std::exchange(rhs._capacity, 0);
    }
    return *this;
  }

  RawBytes &operator=(const RawBytes &) = delete;

  ~RawBytes() { std::free(_buf); }

  void append(const_pointer newDataBeg, const_pointer newDataEnd) {
    if (newDataBeg != newDataEnd) {
      const auto newDataSize = newDataEnd - newDataBeg;
      ensureAvailableCapacity(newDataSize);
      std::memcpy(_buf + _size, newDataBeg, newDataSize);
      _size += newDataSize;
    }
  }

  void append(const_pointer newData, size_type newDataSize) { return append(newData, newData + newDataSize); }

  void assign(const_pointer first, const_pointer last) {
    reserveExponential(last - first);
    _size = last - first;
    std::memcpy(_buf, first, _size);
  }

  void assign(const_pointer first, size_type size) { assign(first, first + size); }

  void assign(std::istream &in) {
    // Read until EOF in one or more large chunks
    for (_size = 0;;) {
      if (_size == _capacity) {
        reallocUp((_capacity * 2U) + 1U);
      }
      const auto nbBytesAvailableInBuf = static_cast<std::streamsize>(_capacity - _size);
      in.read(reinterpret_cast<char *>(std::to_address(_buf + _size)), nbBytesAvailableInBuf);
      const auto nbBytesRead = in.gcount();
      _size += nbBytesRead;
      if (nbBytesRead < nbBytesAvailableInBuf) {  // EOF reached
        break;
      }
    }
  }

  void clear() noexcept { _size = 0; }

  void erase_front(size_type n) {
    assert(n <= _size);
    std::memmove(_buf, _buf + n, _size - n);
    _size -= n;
  }

  void setSize(size_type newSize) {
    assert(newSize <= _capacity);
    _size = newSize;
  }

  [[nodiscard]] size_type size() const noexcept { return _size; }

  [[nodiscard]] size_type capacity() const noexcept { return _capacity; }

  void reserveExponential(size_type newCapacity) {
    if (_capacity < newCapacity) {
      reallocUp(std::max(newCapacity, (_capacity * 2U) + 1U));
    }
  }

  void ensureAvailableCapacity(size_type availableCapacity) { reserveExponential(_size + availableCapacity); }

  [[nodiscard]] pointer data() const noexcept { return _buf; }

  [[nodiscard]] iterator begin() noexcept { return _buf; }
  [[nodiscard]] const_iterator begin() const noexcept { return _buf; }

  [[nodiscard]] iterator end() noexcept { return _buf + _size; }
  [[nodiscard]] const_iterator end() const noexcept { return _buf + _size; }

  [[nodiscard]] bool empty() const noexcept { return _size == 0; }

  void swap(RawBytes &rhs) noexcept {
    using std::swap;
    swap(_buf, rhs._buf);
    swap(_size, rhs._size);
    swap(_capacity, rhs._capacity);
  }

  operator ViewType() const noexcept { return {_buf, _size}; }

  using trivially_relocatable = std::true_type;

 private:
  void reallocUp(size_type newCapacity) {
    pointer newBuf = static_cast<pointer>(std::realloc(_buf, newCapacity));
    if (newBuf == nullptr) {
      throw std::bad_alloc();
    }
    _buf = newBuf;
    _capacity = newCapacity;
  }

  pointer _buf = nullptr;
  size_type _size = 0;
  size_type _capacity = 0;
};

}  // namespace aeronet