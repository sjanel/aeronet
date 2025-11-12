#pragma once

#include <string_view>

#include "aeronet/char-hexadecimal-converter.hpp"

namespace aeronet {

template <class IsNotEncodedFunc>
constexpr auto URLEncodedSize(std::string_view data, IsNotEncodedFunc isNotEncodedFunc) {
  std::string_view::size_type nbChars = 0;

  // not using std::ranges::count_if to avoid <algorithm> include in a header file
  for (char ch : data) {
    if (isNotEncodedFunc(ch)) {
      ++nbChars;
    } else {
      nbChars += 3UL;
    }
  }

  return nbChars;
}

/// This function converts the given input string to a URL encoded string.
/// All input characters 'ch' for which isNotEncodedFunc(ch) is false are converted in upper case hexadecimal.
/// (%NN where NN is a two-digit hexadecimal number).
/// The output is written to the provided 'buf' buffer, which should have enough space to hold the result (at least
/// URLEncodedSize(data, isNotEncodedFunc) bytes). The function returns a pointer to the char immediately after the last
/// written char in the buffer.
template <class IsNotEncodedFunc>
char* URLEncode(std::string_view data, IsNotEncodedFunc isNotEncodedFunc, char* buf) {
  for (char ch : data) {
    if (isNotEncodedFunc(ch)) {
      *buf++ = ch;
    } else {
      *buf = '%';
      buf = to_upper_hex(ch, ++buf);
    }
  }
  return buf;
}

}  // namespace aeronet
