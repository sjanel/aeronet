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

// ConcatenatedStrings stores N string parts in a single contiguous
// RawChars buffer. It provides access to individual parts as string_views or temporary null-terminated strings,
// and allows replacing individual parts while keeping the rest intact.
template <unsigned N, class SizeType = std::size_t>
class StaticConcatenatedStrings {
  static_assert(N > 0U, "StaticConcatenatedStrings requires N > 0");
  static_assert(std::is_unsigned_v<SizeType>, "StaticConcatenatedStrings requires an unsigned SizeType");

 public:
  using size_type = SizeType;

  using offsets_array = std::array<size_type, N - 1>;

  static constexpr offsets_array::size_type kParts = N;

  StaticConcatenatedStrings() noexcept = default;

  StaticConcatenatedStrings(std::initializer_list<std::string_view> parts) {
    if (parts.size() != kParts) {
      throw std::length_error("StaticConcatenatedStrings: must provide exactly the compile-time number of parts");
    }

    uintmax_t total = 0;
    for (auto part : parts) {
      if constexpr (sizeof(size_type) < sizeof(std::string_view::size_type)) {
        if (std::cmp_greater(part.size(), std::numeric_limits<size_type>::max())) {
          throw std::length_error("StaticConcatenatedStrings: part size exceeds maximum");
        }
      }
      total += part.size();
      if constexpr (sizeof(size_type) < sizeof(std::string_view::size_type)) {
        if (std::cmp_greater_equal(total, std::numeric_limits<size_type>::max())) {
          throw std::length_error("StaticConcatenatedStrings: total size exceeds maximum");
        }
      }
    }

    _buf.reserve(static_cast<size_type>(total + 1U));  // +1 for null terminator support if needed

    size_type pos = 0;
    size_type index = 0;
    for (auto part : parts) {
      _buf.unchecked_append(part);
      pos += static_cast<size_type>(part.size());
      if (std::cmp_less(index + size_type{1}, kParts)) {
        _offsets[static_cast<offsets_array::size_type>(index)] = pos;
        ++index;
      }
    }
    _buf[static_cast<size_type>(total)] = '\0';  // do not count null terminator in size
  }

  void set(size_type idx, std::string_view str) {
    const size_type oldBegPos = idx == 0 ? 0 : _offsets[static_cast<offsets_array::size_type>(idx - size_type{1})];
    const size_type oldEndPos = idx + 1 == kParts ? _buf.size() : _offsets[static_cast<offsets_array::size_type>(idx)];
    const size_type oldSize = oldEndPos - oldBegPos;

    if constexpr (sizeof(size_type) < sizeof(std::string_view::size_type)) {
      if (std::cmp_greater(static_cast<std::string_view::size_type>(_buf.size()) -
                               static_cast<std::string_view::size_type>(oldSize) + str.size(),
                           std::numeric_limits<size_type>::max())) {
        throw std::length_error("StaticConcatenatedStrings: new part size exceeds maximum");
      }
    }

    const auto newSize = static_cast<size_type>(str.size());
    const auto tailSize = static_cast<size_type>(_buf.size() - oldEndPos);
    auto *data = _buf.data();

    if (newSize > oldSize) {
      const auto delta = static_cast<size_type>(newSize - oldSize);
      _buf.ensureAvailableCapacity(delta);
      // pointers may have changed after ensureAvailableCapacity
      data = _buf.data();
      // move tail to the right
      std::memmove(data + oldEndPos + delta, data + oldEndPos, static_cast<std::size_t>(tailSize));
      // copy new part
      std::memcpy(data + oldBegPos, str.data(), static_cast<std::size_t>(newSize));
      _buf.addSize(delta);
      // update offsets for subsequent parts
      for (auto offsetIdx = static_cast<typename offsets_array::size_type>(idx); offsetIdx + 1U < kParts; ++offsetIdx) {
        _offsets[offsetIdx] += static_cast<size_type>(delta);
      }
    } else if (newSize < oldSize) {
      const auto delta = static_cast<size_type>(oldSize - newSize);
      // move tail to the left
      std::memmove(data + oldBegPos + newSize, data + oldEndPos, static_cast<std::size_t>(tailSize));
      if (newSize != 0) {
        std::memcpy(data + oldBegPos, str.data(), static_cast<std::size_t>(newSize));
      }
      _buf.setSize(static_cast<size_type>(_buf.size() - delta));
      // update offsets for subsequent parts
      for (auto offsetIdx = static_cast<typename offsets_array::size_type>(idx); offsetIdx + 1U < kParts; ++offsetIdx) {
        _offsets[offsetIdx] -= static_cast<size_type>(delta);
      }
    } else if (newSize != 0) {
      std::memcpy(data + oldBegPos, str.data(), static_cast<std::size_t>(newSize));
    }
  }

  [[nodiscard]] std::string_view operator[](size_type idx) const { return {begPtr(idx), endPtr(idx)}; }

  class TmpNullTerminatedSv {
   public:
    TmpNullTerminatedSv(const TmpNullTerminatedSv &) = delete;
    TmpNullTerminatedSv &operator=(const TmpNullTerminatedSv &) = delete;

    TmpNullTerminatedSv(TmpNullTerminatedSv &&other) = delete;
    TmpNullTerminatedSv &operator=(TmpNullTerminatedSv &&other) = delete;

    // Get a temporary null-terminated c-string.
    [[nodiscard]] const char *c_str() const noexcept { return _begPtr; }

    ~TmpNullTerminatedSv() { _begPtr[_svSz] = _ch; }

   private:
    friend class StaticConcatenatedStrings<N, size_type>;

    // construct and temporarily replace the character at endPtr with '\0'
    TmpNullTerminatedSv(char *begPtr, const char *endPtr)
        : _begPtr(begPtr), _svSz(static_cast<size_type>(endPtr - begPtr)), _ch(*endPtr) {
      _begPtr[_svSz] = '\0';
    }

    char *_begPtr;
    size_type _svSz;
    char _ch;
  };

  // Returns a temporary object that provides a c_str() method to have a null-terminated string.
  // WARNING: do not call other methods on this instance before destruction of returned local object,
  // as it modifies temporarily internal buffer state. The safe usage pattern is to call c_str() from
  // the returned object directly where you need it, to restore the buffer as soon as possible.
  // In C++, the standard guarantees that the destructor of the returned temporary will be called
  // at the end of the full expression in which it was created.
  // Example of a safe usage:
  //  ::SSL_CTX_use_certificate_file(raw, cfg.certFileCstrView().c_str(), SSL_FILETYPE_PEM)
  // Note that this method lies about constness as it modifies internal buffer state, but is marked const
  // for convenience of usage, and if the caller respects the usage pattern it is safe.
  auto makeNullTerminated(size_type idx) const noexcept {
    return TmpNullTerminatedSv(const_cast<char *>(begPtr(idx)), endPtr(idx));
  }

  bool operator==(const StaticConcatenatedStrings<N, size_type> &) const noexcept = default;

 private:
  auto *begPtr(size_type idx) noexcept {
    return _buf.data() +
           static_cast<std::ptrdiff_t>(
               idx == 0 ? 0 : _offsets[static_cast<typename offsets_array::size_type>(idx - size_type{1})]);
  }
  const auto *begPtr(size_type idx) const noexcept {
    return _buf.data() +
           static_cast<std::ptrdiff_t>(
               idx == 0 ? 0 : _offsets[static_cast<typename offsets_array::size_type>(idx - size_type{1})]);
  }

  auto *endPtr(size_type idx) noexcept {
    return _buf.data() +
           static_cast<std::ptrdiff_t>(
               idx + 1 == kParts ? _buf.size() : _offsets[static_cast<typename offsets_array::size_type>(idx)]);
  }
  const auto *endPtr(size_type idx) const noexcept {
    return _buf.data() +
           static_cast<std::ptrdiff_t>(
               idx + 1 == kParts ? _buf.size() : _offsets[static_cast<typename offsets_array::size_type>(idx)]);
  }

  offsets_array _offsets{};
  RawBytesBase<char, std::string_view, size_type> _buf;
};

}  // namespace aeronet
