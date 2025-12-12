#pragma once

#include <charconv>
#include <climits>
#include <concepts>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "aeronet/fixedcapacityvector.hpp"
#include "aeronet/log.hpp"
#include "aeronet/nchars.hpp"

namespace aeronet {

inline auto IntegralToCharVector(std::integral auto val) {
  using Int = decltype(val);

  // +1 for minus, +1 for additional partial ranges coverage
  static constexpr auto kMaxSize = std::numeric_limits<Int>::digits10 + 1 + static_cast<int>(std::is_signed_v<Int>);

  using CharVector = FixedCapacityVector<char, kMaxSize>;

  CharVector ret(static_cast<CharVector::size_type>(nchars(val)));

  // no need to check the return value here, it cannot fail as we sized the vector accordingly
  std::to_chars(ret.data(), ret.data() + ret.size(), val);

  return ret;
}

template <std::integral Integral>
Integral StringToIntegral(const char *begPtr, std::size_t len) {
  // No need to value initialize ret, std::from_chars will set it in case no error is returned
  // And in case of error, exception is thrown instead
  Integral ret;

  const char *endPtr = begPtr + len;
  const auto [ptr, errc] = std::from_chars(begPtr, endPtr, ret);

  if (errc != std::errc()) {
    log::critical("Unable to decode '{}' into integral", std::string_view(begPtr, len));
    throw std::invalid_argument("StringToIntegral conversion failed");
  }

  if (ptr != endPtr) {
    log::error("Only '{}' chars from '{}' decoded into integral '{}'", ptr - begPtr, std::string_view(begPtr, len),
               ret);
  }
  return ret;
}

template <std::integral Integral>
Integral StringToIntegral(std::string_view str) {
  return StringToIntegral<Integral>(str.data(), str.size());
}

constexpr char *AppendIntegralToCharBuf(char *buf, std::integral auto val) {
  static constexpr auto kMaxCharsInt =
      std::max(nchars(std::numeric_limits<decltype(val)>::max()), nchars(std::numeric_limits<decltype(val)>::min()));
  return std::to_chars(buf, buf + kMaxCharsInt, val).ptr;
}

constexpr std::span<char> IntegralToCharBuffer(std::span<char> buf, std::integral auto val) {
  const auto nbDigitsInt = nchars(val);

  if (buf.size() < static_cast<std::size_t>(nbDigitsInt)) {
    throw std::invalid_argument("Buffer size too small for integral conversion");
  }

  std::to_chars(buf.data(), buf.data() + nbDigitsInt, val);

  return buf.subspan(0, nbDigitsInt);
}

}  // namespace aeronet
