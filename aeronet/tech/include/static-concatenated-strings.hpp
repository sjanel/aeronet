#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "raw-chars.hpp"

namespace aeronet {

// ConcatenatedStrings stores N string parts in a single contiguous
// RawChars buffer. It provides access to individual parts as string_views or temporary null-terminated strings,
// and allows replacing individual parts while keeping the rest intact.
template <unsigned N>
class StaticConcatenatedStrings {
  static_assert(N > 0U, "StaticConcatenatedStrings requires N > 0");

 public:
  using size_type = std::size_t;
  using offset_type = uint32_t;

  using offsets_array = std::array<offset_type, N - 1>;

  static constexpr size_type kParts = N;

  StaticConcatenatedStrings() noexcept = default;

  StaticConcatenatedStrings(std::initializer_list<std::string_view> parts) {
    if (parts.size() != kParts) {
      throw std::length_error("StaticConcatenatedStrings: must provide exactly the compile-time number of parts");
    }

    size_type total = 0;
    for (auto part : parts) {
      total += part.size();
    }

    if (total > static_cast<size_type>(std::numeric_limits<offset_type>::max())) {
      throw std::length_error("StaticConcatenatedStrings: concatenated strings too large");
    }

    _buf = RawChars(static_cast<RawChars::size_type>(total) + 1UL);  // +1 for null terminator support if needed

    offset_type pos = 0;
    size_type index = 0;
    for (auto part : parts) {
      _buf.unchecked_append(part);
      pos += static_cast<offset_type>(part.size());
      if (index + 1 < kParts) {
        _offsets[index++] = pos;
      }
    }
    _buf[total] = '\0';  // do not count null terminator in size
  }

  void set(size_type idx, std::string_view str) {
    const auto oldBegPos = static_cast<size_type>(idx == 0 ? 0 : _offsets[idx - 1]);
    const auto oldEndPos = static_cast<size_type>(idx + 1 == kParts ? _buf.size() : _offsets[idx]);
    const auto oldSize = oldEndPos - oldBegPos;
    const auto newSize = str.size();
    const auto tailSize = _buf.size() - oldEndPos;
    auto *data = _buf.data();

    if (newSize > oldSize) {
      const auto delta = newSize - oldSize;
      if (_buf.size() + delta > static_cast<size_type>(std::numeric_limits<offset_type>::max())) {
        throw std::length_error("StaticConcatenatedStrings: concatenated strings too large");
      }
      _buf.ensureAvailableCapacity(delta);
      // pointers may have changed after ensureAvailableCapacity
      data = _buf.data();
      // move tail to the right
      std::memmove(data + oldEndPos + delta, data + oldEndPos, tailSize);
      // copy new part
      std::memcpy(data + oldBegPos, str.data(), newSize);
      _buf.addSize(delta);
      // update offsets for subsequent parts
      for (size_type offsetIdx = idx; offsetIdx + 1 < kParts; ++offsetIdx) {
        _offsets[offsetIdx] += static_cast<offset_type>(delta);
      }
    } else if (newSize < oldSize) {
      const auto delta = oldSize - newSize;
      // move tail to the left
      std::memmove(data + oldBegPos + newSize, data + oldEndPos, tailSize);
      if (newSize != 0) {
        std::memcpy(data + oldBegPos, str.data(), newSize);
      }
      _buf.setSize(_buf.size() - delta);
      // update offsets for subsequent parts
      for (size_type offsetIdx = idx; offsetIdx + 1 < kParts; ++offsetIdx) {
        _offsets[offsetIdx] -= static_cast<offset_type>(delta);
      }
    } else if (newSize != 0) {
      std::memcpy(data + oldBegPos, str.data(), newSize);
    }
  }

  [[nodiscard]] std::string_view operator[](size_type idx) const { return {begPtr(idx), endPtr(idx)}; }

  class TmpNullTerminatedSv {
   public:
    TmpNullTerminatedSv(const TmpNullTerminatedSv &) = delete;
    TmpNullTerminatedSv &operator=(const TmpNullTerminatedSv &) = delete;

    TmpNullTerminatedSv(TmpNullTerminatedSv &&other) noexcept
        : _begPtr(std::exchange(other._begPtr, nullptr)), _svSz(other._svSz), _ch(other._ch) {}

    TmpNullTerminatedSv &operator=(TmpNullTerminatedSv &&other) noexcept {
      if (this != &other) {
        release();
        _begPtr = std::exchange(other._begPtr, nullptr);
        _svSz = other._svSz;
        _ch = other._ch;
      }
      return *this;
    }

    [[nodiscard]] const char *c_str() const noexcept { return _begPtr; }

    ~TmpNullTerminatedSv() { release(); }

   private:
    friend class StaticConcatenatedStrings<N>;

    void release() noexcept {
      if (_begPtr != nullptr) {
        _begPtr[_svSz] = _ch;
      }
    }

    // construct and temporarily replace the character at endPtr with '\0'
    TmpNullTerminatedSv(char *begPtr, char *endPtr)
        : _begPtr(begPtr), _svSz(static_cast<offset_type>(endPtr - begPtr)), _ch(*endPtr) {
      _begPtr[_svSz] = '\0';
    }

    char *_begPtr;
    offset_type _svSz;
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
    return TmpNullTerminatedSv(const_cast<char *>(begPtr(idx)), const_cast<char *>(endPtr(idx)));
  }

 private:
  auto *begPtr(size_type idx) noexcept { return _buf.data() + (idx == 0 ? 0 : _offsets[idx - 1]); }
  const auto *begPtr(size_type idx) const noexcept { return _buf.data() + (idx == 0 ? 0 : _offsets[idx - 1]); }

  auto *endPtr(size_type idx) noexcept { return _buf.data() + (idx + 1 == kParts ? _buf.size() : _offsets[idx]); }
  const auto *endPtr(size_type idx) const noexcept {
    return _buf.data() + (idx + 1 == kParts ? _buf.size() : _offsets[idx]);
  }

  offsets_array _offsets{};
  RawChars _buf;
};

}  // namespace aeronet
