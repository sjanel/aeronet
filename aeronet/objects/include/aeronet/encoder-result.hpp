#pragma once

#include <cassert>
#include <cstdint>

#include "aeronet/safe-cast.hpp"

namespace aeronet {

// Represents the result of an encoding operation, which can either be a success with the number of bytes written, or an
// error.
class EncoderResult {
 public:
  enum class Error : std::int8_t { NotEnoughCapacity = -1, CompressionError = -2 };

  constexpr explicit EncoderResult(Error error) : _written(static_cast<int64_t>(error)) {}

  constexpr explicit EncoderResult(std::size_t written) : _written(SafeCast<int64_t>(written)) {}

  [[nodiscard]] constexpr bool hasError() const noexcept { return _written < 0; }

  [[nodiscard]] constexpr std::size_t written() const noexcept {
    assert(!hasError());
    return static_cast<std::size_t>(_written);
  }

  [[nodiscard]] constexpr Error error() const noexcept {
    assert(hasError());
    return static_cast<Error>(_written);
  }

 private:
  std::int64_t _written;  // negative value indicates error
};

}  // namespace aeronet