#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
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
template <class T, class ViewType = std::span<const T>>
class RawBytesImpl {
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

  RawBytesImpl() noexcept = default;

  explicit RawBytesImpl(size_type capacity)
      : _buf(static_cast<value_type *>(std::malloc(capacity))), _capacity(capacity) {
    if (capacity != 0 && _buf == nullptr) {
      throw std::bad_alloc();
    }
  }

  explicit RawBytesImpl(ViewType data) : RawBytesImpl(data.size()) {
    std::memcpy(_buf, data.data(), _capacity);
    _size = _capacity;
  }

  RawBytesImpl(const_pointer first, const_pointer last) : RawBytesImpl(static_cast<std::size_t>(last - first)) {
    assert(first <= last);
    std::memcpy(_buf, first, _capacity);
    _size = _capacity;
  }

  RawBytesImpl(const RawBytesImpl &) = delete;

  RawBytesImpl(RawBytesImpl &&rhs) noexcept
      : _buf(std::exchange(rhs._buf, nullptr)),
        _size(std::exchange(rhs._size, 0)),
        _capacity(std::exchange(rhs._capacity, 0)) {}

  RawBytesImpl &operator=(RawBytesImpl &&rhs) noexcept {
    if (this != &rhs) {
      std::free(_buf);
      _buf = std::exchange(rhs._buf, nullptr);
      _size = std::exchange(rhs._size, 0);
      _capacity = std::exchange(rhs._capacity, 0);
    }
    return *this;
  }

  RawBytesImpl &operator=(const RawBytesImpl &) = delete;

  ~RawBytesImpl() { std::free(_buf); }

  void unchecked_append(const_pointer first, const_pointer last) {
    const std::size_t sz = static_cast<std::size_t>(last - first);
    std::memcpy(_buf + _size, first, sz);
    _size += sz;
  }

  void unchecked_append(const_pointer newData, size_type newDataSize) {
    return unchecked_append(newData, newData + newDataSize);
  }

  void unchecked_append(ViewType data) { return unchecked_append(data.data(), data.data() + data.size()); }

  void append(const_pointer first, const_pointer last) {
    assert(first <= last);
    ensureAvailableCapacity(static_cast<std::size_t>(last - first));
    unchecked_append(first, last);
  }

  void append(const_pointer newData, size_type newDataSize) { return append(newData, newData + newDataSize); }

  void append(ViewType data) { return append(data.data(), data.data() + data.size()); }

  void unchecked_push_back(value_type byte) { _buf[_size++] = byte; }

  void push_back(value_type byte) {
    ensureAvailableCapacity(1);
    unchecked_push_back(byte);
  }

  void assign(const_pointer first, size_type size) {
    reserveExponential(size);
    std::memcpy(_buf, first, size);
    _size = size;
  }

  void assign(ViewType data) { assign(data.data(), data.size()); }

  void assign(const_pointer first, const_pointer last) { assign(first, last - first); }

  void clear() noexcept { _size = 0; }

  void erase_front(size_type n) {
    assert(n <= _size);
    std::memmove(_buf, _buf + n, _size - n);
    _size -= n;
  }

  void resize_down(size_type newSize) {
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

  void swap(RawBytesImpl &rhs) noexcept {
    using std::swap;
    swap(_buf, rhs._buf);
    swap(_size, rhs._size);
    swap(_capacity, rhs._capacity);
  }

  value_type &operator[](size_type pos) { return _buf[pos]; }
  value_type operator[](size_type pos) const { return _buf[pos]; }

  operator ViewType() const noexcept { return {_buf, _size}; }

  /**
   * resize_and_overwrite (op MUST NOT THROW – undefined behavior otherwise; mirrors std::basic_string) .
   * Ensures capacity for `n`, invokes op(data, n) and sets size() to the returned value (<= n).
   * Common patterns: append (grow then partially fill) or truncate (pass lambda returning n).
   */
  template <class Operation>
  void resize_and_overwrite(size_type n, Operation &&op) {
    reserveExponential(n);
    size_type newSize = static_cast<size_type>(std::forward<Operation>(op)(_buf, n));
    assert(newSize <= n && "resize_and_overwrite: operation returned size larger than requested n");
    _size = newSize;
  }

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

using RawBytes = RawBytesImpl<std::byte, std::span<const std::byte>>;

}  // namespace aeronet