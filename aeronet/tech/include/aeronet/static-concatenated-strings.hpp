#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "aeronet/internal/raw-bytes-base.hpp"

namespace aeronet {

// StaticConcatenatedStrings stores N string parts in a single contiguous RawChars buffer.
// It provides access to individual parts as string_views pointing to null-terminated strings,
// and allows replacing individual parts while keeping the rest intact.
// Compared to its cousin DynamicConcatenatedStrings, it has a fixed number of parts (N) known at compile time,
// which allows for O(1) access to a specific part. Setting a sub-string in the middle that is of different length
// than the previous one requires shifting the tail of the buffer accordingly, which is O(M) with M being
// the size of the tail.
template <unsigned N, class SizeType = std::size_t>
class StaticConcatenatedStrings {
  static_assert(N > 1U, "StaticConcatenatedStrings requires N > 1");
  static_assert(std::is_unsigned_v<SizeType>, "StaticConcatenatedStrings requires an unsigned SizeType");

 public:
  using size_type = SizeType;

  using Offsets = std::array<size_type, N - 1>;

  static constexpr Offsets::size_type kParts = N;

  StaticConcatenatedStrings() noexcept : _offsets(), _buf() {}

  explicit StaticConcatenatedStrings(size_type initialCapacity) : _offsets(), _buf(initialCapacity) {}

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
      _buf.reserve(str.size() + kParts);
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
      _buf.ensureAvailableCapacity(delta);
      // pointers may have changed after ensureAvailableCapacity
      data = _buf.data();
      // move tail to the right
      std::memmove(data + oldEndPos + delta, data + oldEndPos, tailSize);
      // copy new part
      // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
      std::memcpy(data + oldBegPos, str.data(), newSize);
      _buf.addSize(static_cast<size_type>(delta));
      // update offsets for subsequent parts
      for (auto offsetIdx = static_cast<typename Offsets::size_type>(idx); offsetIdx + 1U < kParts; ++offsetIdx) {
        _offsets[offsetIdx] += static_cast<size_type>(delta);
      }
    } else if (newSize < oldSize) {
      const auto delta = static_cast<size_type>(oldSize - newSize);
      // move tail to the left
      std::memmove(data + oldBegPos + newSize, data + oldEndPos, tailSize);
      if (newSize != 0) {
        // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
        std::memcpy(data + oldBegPos, str.data(), newSize);
      }
      _buf.setSize(static_cast<size_type>(_buf.size() - delta));
      // update offsets for subsequent parts
      for (auto offsetIdx = static_cast<typename Offsets::size_type>(idx); offsetIdx + 1U < kParts; ++offsetIdx) {
        _offsets[offsetIdx] -= static_cast<size_type>(delta);
      }
    } else if (newSize != 0) {
      // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
      std::memcpy(data + oldBegPos, str.data(), newSize);
    }
  }

  [[nodiscard]] const char* c_str(size_type idx) const { return begPtr(idx); }

  [[nodiscard]] std::string_view operator[](size_type idx) const { return {begPtr(idx), sizeAt(idx)}; }

  bool operator==(const StaticConcatenatedStrings&) const noexcept = default;

  using trivially_relocatable = std::true_type;

 private:
  const char* begPtr(size_type idx) const noexcept {
    return _buf.data() + static_cast<std::ptrdiff_t>(
                             idx == 0 ? 0 : _offsets[static_cast<typename Offsets::size_type>(idx - size_type{1})]);
  }

  size_type sizeAt(size_type idx) const noexcept {
    if (_buf.empty()) {
      return 0;
    }
    const size_type begPos = idx == 0 ? 0 : _offsets[static_cast<typename Offsets::size_type>(idx - size_type{1})];
    const size_type endPos =
        (idx + 1 == kParts ? _buf.size() : _offsets[static_cast<typename Offsets::size_type>(idx)]) - size_type{1};
    return endPos - begPos;
  }

  Offsets _offsets;
  RawBytesBase<char, std::string_view, size_type> _buf;
};

}  // namespace aeronet
