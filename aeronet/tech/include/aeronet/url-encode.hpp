#pragma once

#include <cstring>
#include <string_view>

#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/compiler-config.hpp"
#include "aeronet/memory-utils.hpp"

namespace aeronet {

template <class IsNotEncodedFunc>
constexpr auto URLEncodedSize(std::string_view data, IsNotEncodedFunc isNotEncodedFunc) {
  std::string_view::size_type nbExtraBytes = 0;

  for (char ch : data) {
    nbExtraBytes += 2 * static_cast<std::string_view::size_type>(!isNotEncodedFunc(ch));
  }

  return data.size() + nbExtraBytes;
}

/// This function converts the given input string to a URL encoded string.
/// All input characters 'ch' for which isNotEncodedFunc(ch) is false are converted in upper case hexadecimal.
/// (%NN where NN is a two-digit hexadecimal number).
/// The output is written to the provided 'buf' buffer, which should have enough space to hold the result (at least
/// URLEncodedSize(data, isNotEncodedFunc) bytes). The function returns a pointer to the char immediately after the last
/// written char in the buffer.
template <class IsNotEncodedFunc>
char* URLEncode(std::string_view data, IsNotEncodedFunc isNotEncodedFunc, char* AERONET_RESTRICT buf) {
  const char* it = data.data();
  const char* const last = it + data.size();

  while (it != last) {
    const char* const runStart = it;
    while (it != last && isNotEncodedFunc(*it)) {
      ++it;
    }

    buf = Append(runStart, static_cast<std::size_t>(it - runStart), buf);

    if (it != last) {
      *buf++ = '%';
      buf = to_upper_hex(static_cast<unsigned char>(*it), buf);
      ++it;
    }
  }

  return buf;
}

}  // namespace aeronet
