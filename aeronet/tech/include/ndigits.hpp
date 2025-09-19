#pragma once

#include <concepts>
#include <limits>

namespace aeronet {

/// Return the number of digits of given integral.
/// The minus sign is not counted - use nchars if you want it counted.
/// Maximum of int(log2(std::numeric_limits<T>::digits10)) + 2 comparisons.
constexpr int ndigits(std::signed_integral auto n) noexcept {
  using T = decltype(n);

  if constexpr (std::numeric_limits<T>::digits10 == 2) {
    return n < 0 ? (n > -10 ? 1 : (n > -100 ? 2 : 3)) : n < 10 ? 1 : (n < 100 ? 2 : 3);
  }

  else if constexpr (std::numeric_limits<T>::digits10 == 4) {
    return n < 0 ? (n > -100 ? (n > -10 ? 1 : 2) : (n > -1000 ? 3 : (n > -10000 ? 4 : 5)))
                 : (n < 100 ? (n < 10 ? 1 : 2) : (n < 1000 ? 3 : (n < 10000 ? 4 : 5)));
  }

  else if constexpr (std::numeric_limits<T>::digits10 == 9) {
    return n < 0 ? (n > -100000
                        ? (n > -1000 ? (n > -10 ? 1 : (n > -100 ? 2 : 3)) : (n > -10000 ? 4 : 5))
                        : (n > -10000000 ? (n > -1000000 ? 6 : 7) : (n > -1000000000 ? (n > -100000000 ? 8 : 9) : 10)))
                 : (n < 100000
                        ? (n < 1000 ? (n < 10 ? 1 : (n < 100 ? 2 : 3)) : (n < 10000 ? 4 : 5))
                        : (n < 10000000 ? (n < 1000000 ? 6 : 7) : (n < 1000000000 ? (n < 100000000 ? 8 : 9) : 10)));
  }

  else if constexpr (std::numeric_limits<T>::digits10 == 18) {
    return n < 0
               ? (n > -1000000000L
                      ? (n > -10000L ? (n > -100L ? (n > -10L ? 1 : 2) : (n > -1000L ? 3 : 4))
                                     : (n > -1000000L ? (n > -100000L ? 5 : 6)
                                                      : (n > -10000000L ? 7 : (n > -100000000L ? 8 : 9))))
                      : (n > -100000000000000L
                             ? (n > -1000000000000L ? (n > -10000000000L ? 10 : (n > -100000000000L ? 11 : 12))
                                                    : (n > -10000000000000L ? 13 : 14))
                             : (n > -10000000000000000L
                                    ? (n > -1000000000000000L ? 15 : 16)
                                    : (n > -1000000000000000000L ? (n > -100000000000000000L ? 17 : 18) : 19))))
               : (n < 1000000000L
                      ? (n < 10000L
                             ? (n < 100L ? (n < 10L ? 1 : 2) : (n < 1000L ? 3 : 4))
                             : (n < 1000000L ? (n < 100000L ? 5 : 6) : (n < 10000000L ? 7 : (n < 100000000L ? 8 : 9))))
                      : (n < 100000000000000L
                             ? (n < 1000000000000L ? (n < 10000000000L ? 10 : (n < 100000000000L ? 11 : 12))
                                                   : (n < 10000000000000L ? 13 : 14))
                             : (n < 10000000000000000L
                                    ? (n < 1000000000000000L ? 15 : 16)
                                    : (n < 1000000000000000000L ? (n < 100000000000000000L ? 17 : 18) : 19))));
  }

  else {
    // Note: below ugly template lambda can be replaced with 'static_assert(false);' in C++23
    []<bool flag = false>() { static_assert(flag, "unknown digits10 value"); }();
  }
}

/// Return the number of digits of given integral.
/// Maximum of int(log2(std::numeric_limits<T>::digits10)) + 1 comparisons.
constexpr int ndigits(std::unsigned_integral auto n) noexcept {
  using T = decltype(n);

  if constexpr (std::numeric_limits<T>::digits10 == 2) {
    return n < 10U ? 1 : (n < 100U ? 2 : 3);
  }

  else if constexpr (std::numeric_limits<T>::digits10 == 4) {
    return n < 100U ? (n < 10U ? 1 : 2) : (n < 1000U ? 3 : (n < 10000U ? 4 : 5));
  }

  else if constexpr (std::numeric_limits<T>::digits10 == 9) {
    return n < 100000U ? (n < 1000U ? (n < 10U ? 1 : (n < 100U ? 2 : 3)) : (n < 10000U ? 4 : 5))
                       : (n < 10000000U ? (n < 1000000U ? 6 : 7) : (n < 1000000000U ? (n < 100000000U ? 8 : 9) : 10));
  }

  else if constexpr (std::numeric_limits<T>::digits10 == 19) {
    return n < 10000000000UL
               ? (n < 1000000UL
                      ? (n < 1000UL ? (n < 100UL ? (n < 10UL ? 1 : 2) : 3) : (n < 100000UL ? (n < 10000UL ? 4 : 5) : 6))
                      : (n < 100000000UL ? (n < 10000000UL ? 7 : 8) : (n < 1000000000UL ? 9 : 10)))
               : (n < 10000000000000000UL
                      ? (n < 10000000000000UL ? (n < 1000000000000UL ? (n < 100000000000UL ? 11 : 12) : 13)
                                              : (n < 1000000000000000UL ? (n < 100000000000000UL ? 14 : 15) : 16))
                      : (n < 1000000000000000000UL ? (n < 100000000000000000UL ? 17 : 18)
                                                   : (n < 10000000000000000000UL ? 19 : 20)));
  }

  else {
    // Note: below ugly template lambda can be replaced with 'static_assert(false);' in C++23
    []<bool flag = false>() { static_assert(flag, "unknown digits10 value"); }();
  }
}

}  // namespace aeronet