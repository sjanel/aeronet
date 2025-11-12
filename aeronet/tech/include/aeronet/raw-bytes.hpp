#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace aeronet {

/**
 * A simple buffer class that manages a dynamically allocated buffer.
 * It is designed to be used by compression libraries (gzip, zstd) that
 * require a simple, low-level buffer interface, do not use it for general-purpose data storage (prefer vector in that
 * case).
 */
template <class T, class ViewType = std::span<const T>, class SizeType = std::size_t>
class RawBytesImpl {
 public:
  using value_type = T;
  using size_type = SizeType;
  using pointer = value_type *;
  using const_pointer = const value_type *;
  using reference = value_type &;
  using const_reference = const value_type &;
  using iterator = value_type *;
  using const_iterator = const value_type *;

  static_assert(std::is_trivially_copyable_v<T> && sizeof(T) == 1);
  static_assert(std::is_unsigned_v<SizeType>, "RawBytesImpl requires an unsigned size type");

  RawBytesImpl() noexcept = default;

  explicit RawBytesImpl(size_type capacity)
      : _buf(static_cast<value_type *>(std::malloc(capacity))), _capacity(capacity) {
    if (capacity != 0 && _buf == nullptr) {
      throw std::bad_alloc();
    }
  }

  explicit RawBytesImpl(ViewType data) : RawBytesImpl(data.size()) {
    if constexpr (sizeof(size_type) < sizeof(uint64_t)) {
      if (std::cmp_less(std::numeric_limits<size_type>::max(), data.size())) {
        throw std::length_error("RawBytesImpl: constructor size exceeds maximum");
      }
    }
    if (!data.empty()) {
      std::memcpy(_buf, data.data(), _capacity);
      _size = _capacity;
    }
  }

  RawBytesImpl(const_pointer first, const_pointer last) : RawBytesImpl(static_cast<size_type>(last - first)) {
    assert(first <= last);
    if constexpr (sizeof(size_type) < sizeof(uint64_t)) {
      if (std::cmp_less(std::numeric_limits<size_type>::max(), last - first)) {
        throw std::length_error("RawBytesImpl: constructor size exceeds maximum");
      }
    }
    if (first != last) {
      std::memcpy(_buf, first, _capacity);
      _size = _capacity;
    }
  }

  RawBytesImpl(const RawBytesImpl &rhs) : RawBytesImpl(rhs.capacity()) {
    _size = rhs.size();
    if (!empty()) {
      std::memcpy(_buf, rhs.data(), _size);
    }
  }

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

  RawBytesImpl &operator=(const RawBytesImpl &rhs) {
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

  ~RawBytesImpl() { std::free(_buf); }

  void unchecked_append(const_pointer first, const_pointer last) {
    if (first != last) {
      const std::size_t sz = static_cast<std::size_t>(last - first);
      std::memcpy(_buf + _size, first, sz);
      _size += static_cast<size_type>(sz);
    }
  }

  void unchecked_append(const_pointer newData, size_type newDataSize) {
    unchecked_append(newData, newData + newDataSize);
  }

  void unchecked_append(ViewType data) { unchecked_append(data.data(), data.data() + data.size()); }

  void append(const_pointer first, const_pointer last) {
    assert(first <= last);
    if constexpr (sizeof(size_type) < sizeof(uint64_t)) {
      if (std::cmp_less(std::numeric_limits<size_type>::max(), last - first)) {
        throw std::length_error("RawBytesImpl: append size exceeds maximum");
      }
    }
    ensureAvailableCapacity(static_cast<size_type>(last - first));
    unchecked_append(first, last);
  }

  void append(const_pointer newData, size_type newDataSize) { append(newData, newData + newDataSize); }

  void append(ViewType data) { append(data.data(), data.data() + data.size()); }

  void unchecked_push_back(value_type byte) { _buf[_size++] = byte; }

  void push_back(value_type byte) {
    ensureAvailableCapacity(1U);
    unchecked_push_back(byte);
  }

  // Growth is exponential.
  void assign(const_pointer first, size_type size) {
    reserveExponential(size);
    if (size != 0) {
      std::memcpy(_buf, first, size);
    }
    _size = size;
  }

  void assign(ViewType data) {
    if constexpr (sizeof(size_type) < sizeof(typename ViewType::size_type)) {
      if (std::cmp_less(std::numeric_limits<size_type>::max(), data.size())) {
        throw std::length_error("RawBytesImpl: assign size exceeds maximum");
      }
    }
    assign(data.data(), static_cast<size_type>(data.size()));
  }

  void assign(const_pointer first, const_pointer last) {
    if constexpr (sizeof(size_type) < sizeof(uint64_t)) {
      if (std::cmp_less(std::numeric_limits<size_type>::max(), last - first)) {
        throw std::length_error("RawBytesImpl: assign size exceeds maximum");
      }
    }
    assign(first, static_cast<size_type>(last - first));
  }

  void clear() noexcept { _size = 0; }

  void erase_front(size_type n) {
    assert(n <= _size);
    if (n != 0) {
      std::memmove(_buf, _buf + n, _size - n);
      _size -= n;
    }
  }

  void setSize(size_type newSize) {
    assert(newSize <= _capacity);
    _size = newSize;
  }

  void addSize(size_type delta) {
    assert(_size + delta <= _capacity);
    _size += delta;
  }

  [[nodiscard]] size_type size() const noexcept { return _size; }

  [[nodiscard]] size_type capacity() const noexcept { return _capacity; }

  void reserveExponential(size_type newCapacity) {
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

  void reserve(size_type newCapacity) {
    if (_capacity < newCapacity) {
      reallocUp(newCapacity);
    }
  }

  // Growth is exponential.
  void ensureAvailableCapacity(size_type availableCapacity) {
    if constexpr (sizeof(size_type) < sizeof(uintmax_t)) {
      static constexpr uintmax_t kMaxCapacity = static_cast<uintmax_t>(std::numeric_limits<size_type>::max());
      if (kMaxCapacity < static_cast<uintmax_t>(_size) + availableCapacity) {
        throw std::bad_alloc();
      }
    }
    reserveExponential(_size + availableCapacity);
  }

  [[nodiscard]] pointer data() noexcept { return _buf; }
  [[nodiscard]] const_pointer data() const noexcept { return _buf; }

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

  bool operator==(const RawBytesImpl &rhs) const noexcept {
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

  /**
   * resize_and_overwrite (op MUST NOT THROW – undefined behavior otherwise; mirrors std::basic_string) .
   * Ensures capacity for `n`, invokes op(data, n) and sets size() to the returned value (<= n).
   * Common patterns: append (grow then partially fill) or truncate (pass lambda returning n).
   * Unlike std::string, the grow strategy is exponential to avoid repeated reallocs on incremental growth.
   */
  template <class Operation>
  void resize_and_overwrite(size_type n, Operation &&op) {
    reserveExponential(n);
    _size = static_cast<size_type>(std::forward<Operation>(op)(_buf, n));
    assert(_size <= n && "resize_and_overwrite: operation returned size larger than requested n");
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

template <class T, class ViewType>
void swap(RawBytesImpl<T, ViewType> &lhs, RawBytesImpl<T, ViewType> &rhs) noexcept {
  lhs.swap(rhs);
}

using RawBytes = RawBytesImpl<std::byte, std::span<const std::byte>>;

}  // namespace aeronet