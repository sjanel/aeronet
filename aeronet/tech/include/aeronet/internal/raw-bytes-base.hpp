#pragma once

#include <type_traits>

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
  using pointer = value_type *;
  using const_pointer = const value_type *;
  using reference = value_type &;
  using const_reference = const value_type &;
  using iterator = value_type *;
  using const_iterator = const value_type *;

  static_assert(std::is_trivially_copyable_v<T> && sizeof(T) == 1);
  static_assert(std::is_unsigned_v<SizeType>, "RawBytesBase requires an unsigned size type");

  RawBytesBase() noexcept = default;

  explicit RawBytesBase(size_type capacity);

  explicit RawBytesBase(ViewType data);

  RawBytesBase(const_pointer first, const_pointer last);

  RawBytesBase(const RawBytesBase &rhs);
  RawBytesBase(RawBytesBase &&rhs) noexcept;

  RawBytesBase &operator=(const RawBytesBase &rhs);
  RawBytesBase &operator=(RawBytesBase &&rhs) noexcept;

  ~RawBytesBase();

  void unchecked_append(const_pointer first, const_pointer last);

  void unchecked_append(const_pointer data, size_type sz) { unchecked_append(data, data + sz); }

  void unchecked_append(ViewType data) { unchecked_append(data.data(), data.data() + data.size()); }

  void append(const_pointer first, const_pointer last);

  void append(const_pointer data, size_type sz) { append(data, data + sz); }

  void append(ViewType data) { append(data.data(), data.data() + data.size()); }

  void unchecked_push_back(value_type byte) { _buf[_size++] = byte; }

  void push_back(value_type byte);

  // Growth is exponential.
  void assign(const_pointer data, size_type size);

  void assign(ViewType data);

  void assign(const_pointer first, const_pointer last);

  void clear() noexcept { _size = 0; }

  void erase_front(size_type n);

  void setSize(size_type newSize);

  void addSize(size_type delta);

  [[nodiscard]] size_type size() const noexcept { return _size; }

  [[nodiscard]] size_type capacity() const noexcept { return _capacity; }

  void reserveExponential(size_type newCapacity);

  void reserve(size_type newCapacity);

  void ensureAvailableCapacity(size_type availableCapacity);

  void ensureAvailableCapacityExponential(size_type availableCapacity);

  [[nodiscard]] pointer data() noexcept { return _buf; }
  [[nodiscard]] const_pointer data() const noexcept { return _buf; }

  [[nodiscard]] iterator begin() noexcept { return _buf; }
  [[nodiscard]] const_iterator begin() const noexcept { return _buf; }

  [[nodiscard]] iterator end() noexcept { return _buf + _size; }
  [[nodiscard]] const_iterator end() const noexcept { return _buf + _size; }

  [[nodiscard]] bool empty() const noexcept { return _size == 0; }

  void swap(RawBytesBase &rhs) noexcept;

  value_type &operator[](size_type pos) { return _buf[pos]; }
  value_type operator[](size_type pos) const { return _buf[pos]; }

  operator ViewType() const noexcept { return {_buf, _size}; }

  bool operator==(const RawBytesBase &rhs) const noexcept;

  using trivially_relocatable = std::true_type;

 private:
  void reallocUp(size_type newCapacity);

  pointer _buf = nullptr;
  size_type _size = 0;
  size_type _capacity = 0;
};

template <class T, class ViewType, class SizeType>
void swap(RawBytesBase<T, ViewType, SizeType> &lhs, RawBytesBase<T, ViewType, SizeType> &rhs) noexcept {
  lhs.swap(rhs);
}

}  // namespace aeronet