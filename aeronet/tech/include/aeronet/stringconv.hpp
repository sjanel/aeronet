#pragma once

#include <charconv>
#include <climits>
#include <concepts>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "aeronet/fixedcapacityvector.hpp"
#include "aeronet/nchars.hpp"

namespace aeronet {

// Logging helpers for StringToIntegral error paths. They are defined out-of-line in stringconv.cpp so
// that the (heavy) logging dependency stays out of this very widely-included header.
// Logs (critical) a full decode failure of 'src' into an integral.
void LogStringToIntegralFailure(std::string_view src);
// Logs (error) a partial decode: only 'decodedCount' chars of 'src' were consumed, yielding 'value'.
void LogStringToIntegralPartialDecode(std::ptrdiff_t decodedCount, std::string_view src, std::string_view value);

inline auto IntegralToCharVector(std::integral auto val) {
  using Int = decltype(val);

  // +1 for minus, +1 for additional partial ranges coverage
  static constexpr auto kMaxSize = std::numeric_limits<Int>::digits10 + 1 + static_cast<int>(std::is_signed_v<Int>);

  using CharVector = FixedCapacityVector<char, kMaxSize>;

  CharVector ret(nchars(val));

  // no need to check the return value here, it cannot fail as we sized the vector accordingly
  std::to_chars(ret.data(), ret.data() + ret.size(), val);

  return ret;
}

template <std::integral Integral>
Integral StringToIntegral(const char* begPtr, std::size_t len) {
  // No need to value initialize ret, std::from_chars will set it in case no error is returned
  // And in case of error, exception is thrown instead
  Integral ret;

  const char* endPtr = begPtr + len;
  const auto [ptr, errc] = std::from_chars(begPtr, endPtr, ret);

  if (errc != std::errc()) {
    LogStringToIntegralFailure(std::string_view(begPtr, len));
    throw std::invalid_argument("StringToIntegral conversion failed");
  }

  if (ptr != endPtr) {
    LogStringToIntegralPartialDecode(ptr - begPtr, std::string_view(begPtr, len),
                                     std::string_view(IntegralToCharVector(ret)));
  }
  return ret;
}

template <std::integral Integral>
Integral StringToIntegral(std::string_view str) {
  return StringToIntegral<Integral>(str.data(), str.size());
}

constexpr char* AppendIntegralToCharBuf(char* buf, std::integral auto val) {
  static constexpr auto kMaxCharsInt =
      std::max(nchars(std::numeric_limits<decltype(val)>::max()), nchars(std::numeric_limits<decltype(val)>::min()));
  return std::to_chars(buf, buf + kMaxCharsInt, val).ptr;
}

}  // namespace aeronet
