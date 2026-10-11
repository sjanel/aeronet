#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "aeronet/internal/raw-bytes-base.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/secure-zero.hpp"

namespace aeronet {

// StaticConcatenatedStrings stores N string parts in a single contiguous RawChars buffer.
// It provides access to individual parts as string_views pointing to null-terminated strings,
// and allows replacing individual parts while keeping the rest intact.
// Compared to its cousin DynamicConcatenatedStrings, it has a fixed number of parts (N) known at compile time,
// which allows for O(1) access to a specific part. Setting a sub-string in the middle that is of different length
// than the previous one requires shifting the tail of the buffer accordingly, which is O(M) with M being
// the size of the tail.
// With Secure = true (strings holding secrets, such as private keys), no copy of the content is ever left in freed or
// unused memory: the buffer is overwritten before it is released (destruction, assignment, growth) and the bytes
// left beyond its end by a shorter string are overwritten too.
template <unsigned N, class SizeType = std::size_t, bool Secure = false>
class StaticConcatenatedStrings {
  static_assert(N > 1U, "StaticConcatenatedStrings requires N > 1");
  static_assert(std::is_unsigned_v<SizeType>, "StaticConcatenatedStrings requires an unsigned SizeType");

 public:
  using size_type = SizeType;
  using value_type = std::string;

  using Offsets = std::array<size_type, N - 1>;

  static constexpr Offsets::size_type kParts = N;

  StaticConcatenatedStrings() noexcept : _offsets(), _buf() {}

  explicit StaticConcatenatedStrings(size_type initialCapacity) : _offsets(), _buf(initialCapacity) {}

  StaticConcatenatedStrings(const StaticConcatenatedStrings&) = default;
  StaticConcatenatedStrings(StaticConcatenatedStrings&&) noexcept = default;

  StaticConcatenatedStrings& operator=(const StaticConcatenatedStrings&)
    requires(!Secure)
  = default;
  StaticConcatenatedStrings& operator=(StaticConcatenatedStrings&&) noexcept
    requires(!Secure)
  = default;

  // The copy is made first, then the previous content is scrubbed with the buffer holding it.
  StaticConcatenatedStrings& operator=(const StaticConcatenatedStrings& other)
    requires(Secure)
  {
    if (this != &other) {
      StaticConcatenatedStrings copy(other);
      swap(copy);
    }
    return *this;
  }

  StaticConcatenatedStrings& operator=(StaticConcatenatedStrings&& other) noexcept
    requires(Secure)
  {
    if (this != &other) {
      scrub();
      _offsets = other._offsets;
      _nextInsertIdx = std::exchange(other._nextInsertIdx, size_type{0});
      _buf = std::move(other._buf);
    }
    return *this;
  }

  ~StaticConcatenatedStrings()
    requires(!Secure)
  = default;

  ~StaticConcatenatedStrings()
    requires(Secure)
  {
    scrub();
  }

  StaticConcatenatedStrings(std::initializer_list<std::string_view> parts) {
    if (parts.size() != kParts) {
      throw std::length_error("StaticConcatenatedStrings: must provide exactly the compile-time number of parts");
    }

    uintmax_t total = kParts;  // +1 for null terminator after each part
    for (std::string_view part : parts) {
      if constexpr (sizeof(size_type) < sizeof(std::string_view::size_type)) {
        if (std::cmp_greater_equal(part.size(), std::numeric_limits<size_type>::max())) {
          throw std::overflow_error("StaticConcatenatedStrings: part size exceeds maximum");
        }
      }
      total += part.size();
      if constexpr (sizeof(size_type) < sizeof(std::string_view::size_type)) {
        if (std::cmp_greater_equal(total, std::numeric_limits<size_type>::max())) {
          throw std::overflow_error("StaticConcatenatedStrings: total size exceeds maximum");
        }
      }
    }

    _buf.reserve(static_cast<size_type>(total));

    size_type pos = 0;
    size_type index = 0;
    for (std::string_view part : parts) {
      _buf.unchecked_append(part);
      _buf.unchecked_push_back('\0');
      pos += static_cast<size_type>(part.size() + 1UL);
      if (std::cmp_less(index + size_type{1}, kParts)) {
        _offsets[static_cast<Offsets::size_type>(index)] = pos;
        ++index;
      }
    }
  }

  void set(size_type idx, std::string_view str) {
    if (_buf.empty()) {
      reserve(str.size() + kParts);
      std::memset(_buf.data(), 0, kParts);
      _buf.setSize(kParts);
      for (size_type i = 1U; i < kParts; ++i) {
        _offsets[static_cast<Offsets::size_type>(i - size_type{1})] = i;
      }
    }
    const size_type oldBegPos = idx == 0 ? 0 : _offsets[static_cast<Offsets::size_type>(idx - size_type{1})];
    const size_type oldEndPos =
        (idx + 1 == kParts ? _buf.size() : _offsets[static_cast<Offsets::size_type>(idx)]) - size_type{1};
    const size_type oldSize = oldEndPos - oldBegPos;

    const std::size_t newSize = str.size();
    const size_type tailSize = _buf.size() - oldEndPos;
    char* data = _buf.data();

    if (oldSize < newSize) {
      const std::size_t delta = newSize - oldSize;
      reserve(_buf.size() + delta);
      // pointers may have changed after reserve
      data = _buf.data();
      // move tail to the right
      std::memmove(data + oldEndPos + delta, data + oldEndPos, tailSize);
      // copy new part
      Copy(str, data + oldBegPos);
      _buf.addSize(static_cast<size_type>(delta));
      // update offsets for subsequent parts
      for (auto offsetIdx = static_cast<Offsets::size_type>(idx); offsetIdx + 1U < kParts; ++offsetIdx) {
        _offsets[offsetIdx] += static_cast<size_type>(delta);
      }
    } else if (newSize < oldSize) {
      const auto delta = static_cast<size_type>(oldSize - newSize);
      // move tail to the left
      std::memmove(data + oldBegPos + newSize, data + oldEndPos, tailSize);
      Copy(str, data + oldBegPos);
      _buf.setSize(static_cast<size_type>(_buf.size() - delta));
      if constexpr (Secure) {
        // The end of the previous content, now beyond the size.
        SecureZero(data + _buf.size(), delta);
      }
      // update offsets for subsequent parts
      for (auto offsetIdx = static_cast<Offsets::size_type>(idx); offsetIdx + 1U < kParts; ++offsetIdx) {
        _offsets[offsetIdx] -= static_cast<size_type>(delta);
      }
    } else {
      Copy(str, data + oldBegPos);
    }
  }

  [[nodiscard]] const char* c_str(size_type idx) const { return begPtr(idx); }

  [[nodiscard]] std::string_view operator[](size_type idx) const { return {begPtr(idx), sizeAt(idx)}; }

  // Overwrite one part in place without changing offsets or capacity. Intended for owned secrets before replacement
  // or destruction.
  void secureClearPart(size_type idx) noexcept {
    if (!_buf.empty()) {
      const size_type begPos = idx == 0 ? 0 : _offsets[static_cast<Offsets::size_type>(idx - size_type{1})];
      SecureZero(_buf.data() + begPos, sizeAt(idx));
    }
  }

  // Number of static parts.
  [[nodiscard]] static constexpr size_type size() noexcept { return static_cast<size_type>(kParts); }

  // Reset all parts to empty strings.
  void clear() noexcept {
    scrub();
    _nextInsertIdx = 0;
    _buf.clear();
  }

  void swap(StaticConcatenatedStrings& other) noexcept {
    std::swap(_offsets, other._offsets);
    std::swap(_nextInsertIdx, other._nextInsertIdx);
    _buf.swap(other._buf);
  }

  // JSON set-like path uses range value type (std::string_view) rather than value_type.
  void emplace(std::string_view str) {
    if (_nextInsertIdx >= static_cast<size_type>(kParts)) {
      throw std::length_error("StaticConcatenatedStrings::emplace: too many parts");
    }
    set(_nextInsertIdx, str);
    ++_nextInsertIdx;
  }

  class iterator {
   public:
    using value_type = std::string_view;
    using reference = std::string_view;
    using pointer = const std::string_view*;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    iterator() noexcept = default;

    iterator(const StaticConcatenatedStrings* owner, size_type index) noexcept : _owner(owner), _index(index) {}

    reference operator*() const noexcept { return (*_owner)[_index]; }

    iterator& operator++() noexcept {
      ++_index;
      return *this;
    }

    iterator operator++(int) noexcept {
      iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    bool operator==(const iterator&) const noexcept = default;

   private:
    const StaticConcatenatedStrings* _owner = nullptr;
    size_type _index = 0;
  };

  [[nodiscard]] iterator begin() noexcept { return iterator(this, 0); }
  [[nodiscard]] iterator end() noexcept { return iterator(this, static_cast<size_type>(kParts)); }
  [[nodiscard]] iterator begin() const noexcept { return iterator(this, 0); }
  [[nodiscard]] iterator end() const noexcept { return iterator(this, static_cast<size_type>(kParts)); }

  bool operator==(const StaticConcatenatedStrings& other) const noexcept {
    return _offsets == other._offsets && _buf == other._buf;
  }

  using trivially_relocatable = std::true_type;

 private:
  const char* begPtr(size_type idx) const noexcept {
    return _buf.data() +
           static_cast<std::ptrdiff_t>(idx == 0 ? 0 : _offsets[static_cast<Offsets::size_type>(idx - size_type{1})]);
  }

  // Ensures a capacity of at least newCapacity bytes. A Secure buffer is never reallocated in place (realloc would free
  // the previous memory without scrubbing it): its content is copied to a new buffer, then scrubbed.
  void reserve(std::size_t newCapacity) {
    if constexpr (Secure) {
      if (newCapacity > _buf.capacity() && !_buf.empty()) {
        decltype(_buf) newBuf(newCapacity);
        newBuf.unchecked_append(_buf.data(), _buf.size());
        scrub();
        _buf.swap(newBuf);
        return;
      }
    }
    _buf.reserve(newCapacity);
  }

  // Overwrites the content of a Secure buffer (the bytes beyond its size never hold any of it).
  void scrub() noexcept {
    if constexpr (Secure) {
      if (!_buf.empty()) {
        SecureZero(_buf.data(), _buf.size());
      }
    }
  }

  size_type sizeAt(size_type idx) const noexcept {
    if (_buf.empty()) {
      return 0;
    }
    const size_type begPos = idx == 0 ? 0 : _offsets[static_cast<Offsets::size_type>(idx - size_type{1})];
    const size_type endPos =
        (idx + 1 == kParts ? _buf.size() : _offsets[static_cast<Offsets::size_type>(idx)]) - size_type{1};
    return endPos - begPos;
  }

  Offsets _offsets;
  size_type _nextInsertIdx = 0;
  RawBytesBase<char, std::string_view, size_type> _buf;
};

}  // namespace aeronet
