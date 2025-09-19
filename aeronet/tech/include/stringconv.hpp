#pragma once

#include <charconv>
#include <concepts>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>

#include "exception.hpp"
#include "fixedcapacityvector.hpp"
#include "nchars.hpp"
#include "string.hpp"

namespace aeronet {

namespace details {
inline void ToChars(char *first, std::integral auto sz, std::integral auto val) {
  const auto [ptr, errc] = std::to_chars(first, first + sz, val);
  if (errc != std::errc() || ptr != first + sz) {
    throw exception("Unable to decode integral into string");
  }
}
}  // namespace details

inline string IntegralToString(std::integral auto val) {
  const auto nbDigitsInt = nchars(val);

  string str(nbDigitsInt, '0');

  details::ToChars(str.data(), nbDigitsInt, val);

  return str;
}

inline auto IntegralToCharVector(std::integral auto val) {
  using Int = decltype(val);

  // +1 for minus, +1 for additional partial ranges coverage
  static constexpr auto kMaxSize = std::numeric_limits<Int>::digits10 + 1 + (std::is_signed_v<Int> ? 1 : 0);

  using CharVector = FixedCapacityVector<char, kMaxSize>;

  CharVector ret(static_cast<CharVector::size_type>(nchars(val)));

  char *begPtr = ret.data();
  char *endPtr = begPtr + ret.size();

  const auto [ptr, errc] = std::to_chars(begPtr, endPtr, val);
  if (errc != std::errc() || ptr != endPtr) {
    throw exception("Unable to decode integral {} into string", val);
  }

  return ret;
}

template <std::integral Integral = int>
Integral StringToIntegral(std::string_view str) {
  // No need to value initialize ret, std::from_chars will set it in case no error is returned
  // And in case of error, exception is thrown instead
  Integral ret;

  const char *begPtr = str.data();
  const char *endPtr = begPtr + str.size();
  const auto [ptr, errc] = std::from_chars(begPtr, endPtr, ret);

  if (errc != std::errc()) {
    if (errc == std::errc::result_out_of_range) {
      throw exception("'{}' would produce an out of range integral", str);
    }
    throw exception("Unable to decode '{}' into integral", str);
  }

  if (ptr != endPtr) {
    throw exception("Only {} chars from {} decoded into integral {}", ptr - begPtr, str, ret);
  }
  return ret;
}

inline void AppendIntegralToString(string &str, std::integral auto val) {
  const auto nbDigitsInt = nchars(val);

  str.append(nbDigitsInt, '0');

  details::ToChars(str.data() + static_cast<decltype(nbDigitsInt)>(str.size()) - nbDigitsInt, nbDigitsInt, val);
}

constexpr std::span<char> IntegralToCharBuffer(std::span<char> buf, std::integral auto val) {
  const auto nbDigitsInt = nchars(val);

  if (buf.size() < static_cast<std::size_t>(nbDigitsInt)) {
    throw exception("Buffer size {} is too small to hold {} digits for integral {}", buf.size(), nbDigitsInt, val);
  }

  details::ToChars(buf.data(), nbDigitsInt, val);

  return buf.subspan(0, nbDigitsInt);
}

}  // namespace aeronet