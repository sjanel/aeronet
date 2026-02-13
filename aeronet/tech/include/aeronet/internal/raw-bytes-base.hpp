#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>

#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
#include <cassert>
#include <cstring>
#endif

#include "aeronet/safe-cast.hpp"

namespace aeronet {

/**
 * A simple buffer class that manages a dynamically allocated buffer.
 * It is designed to be used by compression libraries (gzip, zstd) that
 * require a simple, low-level buffer interface, do not use it for general-purpose data storage (prefer vector in that
 * case).
 */
template <class T, class ViewType, class SizeType>
class RawBytesBase {
 public:
  using value_type = T;
  using size_type = SizeType;
  using pointer = value_type*;
  using const_pointer = const value_type*;
  using reference = value_type&;
  using const_reference = const value_type&;
  using iterator = value_type*;
  using const_iterator = const value_type*;
  using view_type = ViewType;

  static_assert(std::is_trivially_copyable_v<T> && sizeof(T) == 1);
  static_assert(std::is_unsigned_v<SizeType>, "RawBytesBase requires an unsigned size type");

  // Constructs an empty buffer, without any allocated storage.
  RawBytesBase() noexcept = default;

  // Constructs an empty buffer with the specified capacity.
  // Warning: unlike std::string or std::vector, the size is set to 0, not to capacity.
  explicit RawBytesBase(uint64_t capacity);

  // Constructs a buffer initialized with the specified data.
  explicit RawBytesBase(ViewType data) : RawBytesBase(data.data(), SafeCast<size_type>(data.size())) {}

  // Constructs a buffer initialized with the specified data.
  RawBytesBase(const_pointer data, uint64_t sz);

  RawBytesBase(const RawBytesBase& rhs);
  RawBytesBase(RawBytesBase&& rhs) noexcept;

  RawBytesBase& operator=(const RawBytesBase& rhs);
  RawBytesBase& operator=(RawBytesBase&& rhs) noexcept;

  ~RawBytesBase();

  // Appends data to the end of the buffer without checking capacity.
  void unchecked_append(const_pointer data, uint64_t sz);

  // Appends data to the end of the buffer without checking capacity.
  void unchecked_append(ViewType data) { unchecked_append(data.data(), SafeCast<size_type>(data.size())); }

  // Appends data to the end of the buffer, reallocating if necessary.
  void append(const_pointer data, uint64_t sz);

  // Appends data to the end of the buffer, reallocating if necessary.
  void append(ViewType data) { append(data.data(), SafeCast<size_type>(data.size())); }

  // Appends a single byte to the end of the buffer without checking capacity.
  void unchecked_push_back(value_type byte) noexcept {
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
    assert(_size < _capacity);
#endif
    _buf[_size++] = byte;
  }

  // Appends a single byte to the end of the buffer, reallocating if necessary.
  void push_back(value_type byte);

  // Assigns new data to the buffer, replacing its current contents.
  void assign(const_pointer data, uint64_t size);

  // Assigns new data to the buffer, replacing its current contents.
  void assign(ViewType data) { assign(data.data(), SafeCast<size_type>(data.size())); }

  // Clears the buffer, setting its size to zero.
  void clear() noexcept {
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
    if (_size != 0) {
      std::memset(_buf, 255, _size);
    }
#endif
    _size = 0;
  }

  // Erases the first n bytes from the buffer.
  void erase_front(size_type n) noexcept;

  // Sets the size of the buffer.
  void setSize(size_type newSize) noexcept {
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
    assert(newSize <= _capacity);
#endif
    _size = newSize;
  }

  // Increases the size of the buffer by delta.
  void addSize(size_type delta) noexcept {
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
    assert(delta <= _capacity - _size);
#endif
    _size += delta;
  }

  // Adjusts the size of the buffer by a signed delta (can shrink or grow).
  void adjustSize(int64_t delta) noexcept {
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
    if (delta > 0) {
      assert(static_cast<size_type>(delta) <= _capacity - _size);
    } else if (delta < 0) {
      assert(static_cast<size_type>(-delta) <= _size);
    }
#endif
    _size += static_cast<size_type>(delta);
  }

  // Returns the current size of the buffer.
  [[nodiscard]] size_type size() const noexcept { return _size; }

  // Returns the current capacity of the buffer.
  [[nodiscard]] size_type capacity() const noexcept { return _capacity; }

  // Returns the available capacity of the buffer.
  [[nodiscard]] size_type availableCapacity() const noexcept { return _capacity - _size; }

  // Reserves capacity for at least newCapacity bytes.
  void reserve(uint64_t newCapacity);

  // Heuristically reduces unused capacity.
  // The current implementation algorithm is to halve the capacity if the size is less than a quarter of the capacity.
  void shrink_to_fit() noexcept;

  // Ensures that the buffer has at least the specified available capacity.
  void ensureAvailableCapacity(uint64_t availableCapacity) { reserve(availableCapacity + _size); }

  // Overload to accept int64_t for convenience.
  void ensureAvailableCapacity(int64_t capa) {
    if (capa > 0) {
      ensureAvailableCapacity(static_cast<uint64_t>(capa));
    }
  }

  // Ensures that the buffer has at least the specified available capacity,
  // growing the capacity exponentially.
  void ensureAvailableCapacityExponential(uint64_t availableCapacity);

  // Overload to accept int64_t for convenience.
  void ensureAvailableCapacityExponential(int64_t capa) {
    if (capa > 0) {
      ensureAvailableCapacityExponential(static_cast<uint64_t>(capa));
    }
  }

  // Returns a pointer to the buffer data.
  [[nodiscard]] pointer data() noexcept { return _buf; }

  // Returns a const pointer to the buffer data.
  [[nodiscard]] const_pointer data() const noexcept { return _buf; }

  // Returns a const iterator to the beginning of the buffer.
  [[nodiscard]] iterator begin() noexcept { return _buf; }

  // Returns a const iterator to the beginning of the buffer.
  [[nodiscard]] const_iterator begin() const noexcept { return _buf; }

  // Returns a const iterator to the end of the buffer.
  [[nodiscard]] iterator end() noexcept { return _buf + _size; }

  // Returns a const iterator to the end of the buffer.
  [[nodiscard]] const_iterator end() const noexcept { return _buf + _size; }

  // Returns true if the buffer is empty.
  [[nodiscard]] bool empty() const noexcept { return _size == 0; }

  // Swaps the contents of this buffer with another buffer.
  void swap(RawBytesBase& rhs) noexcept;

  value_type& operator[](size_type pos) {
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
    assert(pos < _capacity);
#endif
    return _buf[pos];
  }
  value_type operator[](size_type pos) const {
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
    assert(pos < _capacity);
#endif
    return _buf[pos];
  }

  template <class V = ViewType>
    requires std::same_as<V, std::string_view>
  operator ViewType() const noexcept {
    return {_buf, _size};
  }

  bool operator==(const RawBytesBase& rhs) const noexcept;

  using trivially_relocatable = std::true_type;

 private:
  void reallocUp(size_type newCapacity);

  pointer _buf = nullptr;
  size_type _size = 0;
  size_type _capacity = 0;
};

template <class T, class ViewType, class SizeType>
void swap(RawBytesBase<T, ViewType, SizeType>& lhs, RawBytesBase<T, ViewType, SizeType>& rhs) noexcept {
  lhs.swap(rhs);
}

}  // namespace aeronet