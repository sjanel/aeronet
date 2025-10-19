#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "raw-chars.hpp"

namespace aeronet {

// ConcatenatedStrings stores N string parts in a single contiguous
// RawChars buffer and an std::array of uint32_t offsets.
template <unsigned N>
class ConcatenatedStrings {
  static_assert(N > 0U, "ConcatenatedStrings requires N > 0");

 public:
  using size_type = std::size_t;
  using offset_type = uint32_t;

  using offsets_array = std::array<offset_type, N - 1>;

  static constexpr size_type kParts = N;

  ConcatenatedStrings() noexcept = default;

  ConcatenatedStrings(std::initializer_list<std::string_view> parts) {
    if (parts.size() != kParts) {
      throw std::length_error("ConcatenatedStrings: must provide exactly the compile-time number of parts");
    }

    size_type total = 0;
    for (auto part : parts) {
      total += part.size();
    }

    if (total > static_cast<size_type>(std::numeric_limits<offset_type>::max())) {
      throw std::length_error("ConcatenatedStrings: concatenated strings too large");
    }

    _buf = RawChars(static_cast<RawChars::size_type>(total));

    offset_type pos = 0;
    size_type index = 0;
    for (auto part : parts) {
      _buf.unchecked_append(part);
      pos += static_cast<offset_type>(part.size());
      if (index + 1 < kParts) {
        _offsets[index++] = pos;
      }
    }
  }

  [[nodiscard]] std::string_view operator[](size_type idx) const {
    return {_buf.data() + (idx == 0 ? 0 : _offsets[idx - 1]),
            _buf.data() + (idx + 1 == kParts ? _buf.size() : _offsets[idx])};
  }

 private:
  offsets_array _offsets{};
  RawChars _buf;
};

}  // namespace aeronet
